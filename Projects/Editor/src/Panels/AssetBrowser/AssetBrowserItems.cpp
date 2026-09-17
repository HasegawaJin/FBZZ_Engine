/// @file    AssetBrowserItems.cpp
/// @brief   AssetBrowser のフォルダツリーとファイルアイコン描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "AssetBrowserCommon.hpp"
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Editor/Util/DragDropSet.hpp>
#include <Editor/Util/IcoImage.hpp>
#include <Editor/Util/EditorIcons.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Windows.h>
#include <toml++/toml.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Util/Uuid.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

namespace fbzz::editor {
namespace {

/// @brief アセット本体と "<本体>.meta" サイドカーを一括で移動 / リネームし、GUID 索引を追随させる。
/// @brief .meta を置き去りにすると移動先で guid が再発行され、guid: 参照が全て切れる。
/// @brief ディレクトリ移動時は OnAssetMoved が配下の索引をプレフィックス付け替えで追随させる。
bool MoveAssetWithSidecar(const std::string& fromAbs, const std::string& toAbs)
{
    if (fromAbs.empty() || toAbs.empty() ||
        util::FileSystem::SamePathText(fromAbs, toAbs) ||
        !util::FileSystem::Exists(fromAbs) || util::FileSystem::Exists(toAbs))
        return false;

    const std::string fromMeta = fromAbs + ".meta";
    const std::string toMeta = toAbs + ".meta";
    const bool hasMeta = util::FileSystem::Exists(fromMeta);
    /// @note 本体だけ移動して既存の .meta と結び付くと GUID の所有者が変わるため、先に拒否する。
    if (util::FileSystem::Exists(toMeta)) return false;

    if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(fromAbs),
                                  util::FileSystem::PathFromUtf8(toAbs)))
        return false;

    if (hasMeta) {
        if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(fromMeta),
                                      util::FileSystem::PathFromUtf8(toMeta))) {
            /// @note サイドカーを移せない場合は本体を元へ戻し、半端な移動を残さない。
            if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(toAbs),
                                          util::FileSystem::PathFromUtf8(fromAbs)))
                FBZZ_LOG_ERROR("AssetBrowser: move rollback failed [%s]", toAbs.c_str());
            FBZZ_LOG_WARN("AssetBrowser: sidecar move failed [%s]", fromMeta.c_str());
            return false;
        }
    }

    asset::AssetDatabase::OnAssetMoved(fromAbs, toAbs);
    return true;
}

/// @brief 削除したアセットを «もう無いもの» として各所へ知らせる。
/// @note ResourceManager のテクスチャキャッシュはパス一致で即返すので、消しただけだと
///       再起動まで古い絵が出続け、消したつもりのアセットを配布物へ持ち込むことになる。
///       フォルダを渡された場合も配下ごと外れる (どちらの経路も前方一致で処理する)。
void ForgetDeletedAsset(const std::string& absPath, EditorContext& ctx)
{
    /// @note GUID 索引から外す。以後この参照は「解決できない guid」になり、
    ///       読み込み側が壊れた参照として扱えるようになる。
    asset::AssetDatabase::OnAssetRemoved(absPath);

    /// @note キャッシュのキーは Assets/ 起点の相対パス。projectRoot 分を落として合わせる。
    std::string relative = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(absPath));
    std::replace(relative.begin(), relative.end(), '\\', '/');
    std::string root = ctx.projectRoot;
    std::replace(root.begin(), root.end(), '\\', '/');
    if (!root.empty() && relative.rfind(root, 0) == 0) {
        relative.erase(0, root.size());
        while (!relative.empty() && relative.front() == '/') relative.erase(0, 1);
    }
    if (relative.empty()) return;

    if (auto* resources = renderer::ResourceManager::Active()) {
        if (const std::size_t evicted = resources->EvictTexture(relative); evicted > 0) {
            FBZZ_LOG_INFO("AssetBrowser: evicted %zu cached texture(s) under [%s]",
                          evicted, relative.c_str());
        }
    }

    /// @note .mat はテクスチャ参照を抱えたまま別ストアに載っている。パス一致で外す。
    const std::string lowerExt = util::StringUtils::ToLower(util::FileSystem::GetExtension(relative));
    if (lowerExt == ".mat") asset::AssetManager::Unload<asset::MaterialAsset>(relative);
}

/// @brief 削除は Undo 履歴へ載せず、プロジェクト内のごみ箱 (.fbzz/Trash/<日時>/) へ退避する。
/// @note Undo スタックはシーン編集と共有なので、載せると Scene View の Ctrl+Z でディスク上の
///       ファイルが復活・再削除され、履歴からあふれた時点で復元手段も消える。ごみ箱の実体は
///       自動削除しないので、復元はエクスプローラーで戻すだけで済む。
/// @return ごみ箱へ移せた項目数
std::size_t TrashAssets(const std::vector<std::string>& paths, EditorContext& ctx)
{
    if (paths.empty()) return 0;

    /// @note 退避先は 1 回の削除操作につき 1 フォルダ。複数選択の削除をまとめて戻せるようにする。
    const std::time_t now = std::time(nullptr);
    std::tm           local{};
    localtime_s(&local, &now);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const std::string trashRoot =
        ctx.projectRoot + "/.fbzz/Trash/" + stamp + "-" + util::GenerateUUID().substr(0, 8) + "/";

    std::size_t moved = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!util::FileSystem::Exists(paths[i])) continue;

        const std::string dest =
            trashRoot + std::to_string(i) + "_" + util::FileSystem::GetFilename(paths[i]);
        util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(dest));
        if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(paths[i]),
                                      util::FileSystem::PathFromUtf8(dest))) {
            FBZZ_LOG_ERROR("AssetBrowser: delete failed [%s]", paths[i].c_str());
            continue;
        }
        ++moved;

        /// @note .meta サイドカーも一緒に退避する。本体だけ消すと孤児 .meta が残るが、
        ///       ペアで移せば手で戻したときに guid も戻る。
        const std::string metaPath = paths[i] + ".meta";
        if (util::FileSystem::Exists(metaPath)) {
            util::FileSystem::Rename(util::FileSystem::PathFromUtf8(metaPath),
                                     util::FileSystem::PathFromUtf8(dest + ".meta"));
        }

        ForgetDeletedAsset(paths[i], ctx);
    }

    if (moved > 0) {
        FBZZ_LOG_INFO("AssetBrowser: moved %zu item(s) to %s", moved, trashRoot.c_str());
        Toast::Info("Deleted " + std::to_string(moved) + " item(s) \xe2\x86\x92 .fbzz/Trash");
    }
    ctx.requestAssetBrowserRefresh = true;
    return moved;
}

std::string UniqueDuplicatePath(const std::string& srcPath, bool isDir)
{
    const std::string dir = util::FileSystem::GetDirectory(srcPath);
    const std::string name = isDir
        ? util::FileSystem::GetFilename(srcPath)
        : std::filesystem::path(srcPath).stem().string();
    const std::string ext = isDir ? std::string{} : util::StringUtils::ToLower(
        util::FileSystem::GetExtension(srcPath));

    for (int n = 2; n <= 999; ++n) {
        const std::string dstPath = util::FileSystem::NormalizePathSeparators(
            dir + name + "(" + std::to_string(n) + ")" + ext);
        if (!util::FileSystem::Exists(dstPath))
            return dstPath;
    }
    return {};
}

/// @brief UniqueDuplicatePath は常に「元と同じフォルダ」に採番先を作る (その場複製用)。
/// @brief Ctrl+V は別フォルダへ貼り付けることが多いため、まず同名そのままを試し、
/// @brief 衝突する場合だけ "(2)" 以降を採番する。
std::string UniqueDestPath(const std::string& srcPath, const std::string& destDir, bool isDir)
{
    const std::string name = isDir
        ? util::FileSystem::GetFilename(srcPath)
        : std::filesystem::path(srcPath).stem().string();
    const std::string ext = isDir ? std::string{} : util::StringUtils::ToLower(
        util::FileSystem::GetExtension(srcPath));
    std::string dir = util::FileSystem::NormalizePathSeparators(destDir);
    if (!dir.empty() && dir.back() != '/') dir += "/";

    const std::string plain = util::FileSystem::NormalizePathSeparators(dir + name + ext);
    if (util::FileSystem::NormalizePathSeparators(srcPath) != plain &&
        !util::FileSystem::Exists(plain))
        return plain;

    for (int n = 2; n <= 999; ++n) {
        const std::string candidate = util::FileSystem::NormalizePathSeparators(
            dir + name + "(" + std::to_string(n) + ")" + ext);
        if (!util::FileSystem::Exists(candidate))
            return candidate;
    }
    return {};
}

/// @brief 複製で持ち込まれた .meta の guid を振り直す。
/// @note .meta は guid だけでなく importer 設定 (sRGB / 圧縮 / 生成フラグ) を持つため捨てない。
///       捨てれば複製は既定設定で再インポートされて見た目が変わり、そのまま複写すれば複製側の
///       guid が原本と衝突して «複製への参照が原本へ吸われる»。設定は残し guid だけ新しくする。
void ReassignCopiedGuid(const std::string& assetAbsPath)
{
    if (!util::FileSystem::Exists(assetAbsPath + ".meta")) return;
    std::string newGuid;
    if (!asset::AssetDatabase::ReassignGuid(assetAbsPath, newGuid))
        FBZZ_LOG_WARN("AssetBrowser: cannot reassign guid for copy [%s]", assetAbsPath.c_str());
}

/// @brief フォルダ複製は std::filesystem::copy が配下の .meta ごと複写するため、
/// @brief 中身のすべてが GUID 重複になる。複製し終えた «複製先» を舐めて振り直す。
void ReassignCopiedGuidsRecursive(const std::string& dirAbsPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::recursive_directory_iterator it(util::FileSystem::PathFromUtf8(dirAbsPath),
                                        fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator last;
    while (!ec && it != last) {
        const std::string p = util::FileSystem::NormalizePathSeparators(
            util::FileSystem::PathToUtf8(it->path()));
        if (p.size() > 5 && util::StringUtils::ToLower(p.substr(p.size() - 5)) == ".meta")
            ReassignCopiedGuid(p.substr(0, p.size() - 5));
        it.increment(ec);
    }
    if (ec)
        FBZZ_LOG_WARN("AssetBrowser: copy scan stopped [%s]", ec.message().c_str());
}

bool CopyAssetPath(const std::string& srcPath, const std::string& dstPath, bool isDir)
{
    if (srcPath.empty() || dstPath.empty()) return false;

    if (isDir) {
        if (!util::FileSystem::CopyDirectoryRecursive(
                util::FileSystem::PathFromUtf8(srcPath),
                util::FileSystem::PathFromUtf8(dstPath)))
            return false;
        ReassignCopiedGuidsRecursive(dstPath);
        return true;
    }

    if (!util::FileSystem::CopyFile(util::FileSystem::PathFromUtf8(srcPath),
                                    util::FileSystem::PathFromUtf8(dstPath)))
        return false;

    /// @note FBX だけはサイドカーを持ち込まない。原本の隣の .meta は «Import 済み» の印で、
    ///       複製に付けて回ると Baked 生成物を持たないまま Import 済みに見える (AssetDatabase.hpp)。
    if (util::StringUtils::ToLower(util::FileSystem::GetExtension(srcPath)) != ".fbx"
        && util::FileSystem::Exists(srcPath + ".meta")) {
        util::FileSystem::CopyFile(util::FileSystem::PathFromUtf8(srcPath + ".meta"),
                                   util::FileSystem::PathFromUtf8(dstPath + ".meta"));
        ReassignCopiedGuid(dstPath);
    }
    return true;
}

std::string ResolveMoveSourcePath(const std::string& payloadPath, const EditorContext& ctx)
{
    const std::string normalized = util::FileSystem::NormalizePathSeparators(payloadPath);
    if (normalized.empty()) return {};

    /// @note 外部マウントの ASSET_PATH は絶対パスのまま渡されるため、projectRoot を二重付与しない。
    if (util::FileSystem::PathFromUtf8(normalized).is_absolute()) return normalized;
    return ToProjectAssetDiskPath(ctx.projectRoot, normalized);
}

bool MoveProjectAssetToDirectory(const std::string& srcProjectPath,
                                 const std::string& dstDir,
                                 EditorContext& ctx,
                                 std::string& outSrcAbs,
                                 std::string& outDstAbs)
{
    if (srcProjectPath.empty() || dstDir.empty()) return false;

    const std::string srcAbs = ResolveMoveSourcePath(srcProjectPath, ctx);
    const std::string dstAbs = util::FileSystem::NormalizePathSeparators(
        dstDir + "/" + util::FileSystem::GetFilename(srcAbs));
    if (srcAbs.empty() || !util::FileSystem::IsDirectory(dstDir) ||
        util::FileSystem::SamePathText(srcAbs, dstAbs) ||
        util::FileSystem::Exists(dstAbs) || util::FileSystem::Exists(dstAbs + ".meta"))
        return false;
    if (util::FileSystem::IsDirectory(srcAbs) &&
        util::FileSystem::IsChildPathText(dstDir, srcAbs))
        return false;

    if (!MoveAssetWithSidecar(srcAbs, dstAbs)) {
        FBZZ_LOG_ERROR("Move failed: %s -> %s", srcAbs.c_str(), dstAbs.c_str());
        return false;
    }

    /// @note 移動も Undo 履歴には積まない。ファイルの場所はディスクの状態で、シーン編集の
    ///       履歴とは別の軸にある (同じスタックだと Ctrl+Z がアセットを勝手に動かす)。
    ctx.requestAssetBrowserRefresh = true;

    outSrcAbs = srcAbs;
    outDstAbs = dstAbs;
    return true;
}

/// @brief 未知の拡張子は拡張子文字列のハッシュから色を生成し、
///      追加のコード変更なしにどんなファイルでも識別色が付く。
struct ExtGroup {
    const char*  exts[6];   ///< @brief 最大 6 拡張子。nullptr 終番。
    ImVec4       color;
    const char*  label;
};

/// @brief 種別色は 8 つのファミリー + 無彩色に畳んである。
/// @note 拡張子ごとに色を分けないのは、人が確実に見分けられるカテゴリ色は 6〜8 程度で 26 色は
///       覚えられないため。近い色どうしは «区別できるはず» と目に思わせて実際には解像できず、
///       同じ色にするより悪い (旧テーブルは .prefab と .mat が RGB 距離 0.087、.ttf と .fnt が
///       0.173 で同じフォルダで見分けられなかった)。ファミリー内は同じ色にし、細かい種類は
///       アイコン内のラベル (ANIM / MASK …) が示す。色相は円周にほぼ等間隔で、どの 2 色も
///       RGB 距離 0.30 以上を確保する。
static constexpr ImVec4 kFamLook  { 0.23f, 0.62f, 0.82f, 1.0f }; ///< @brief h=200 マテリアル / テクスチャ
static constexpr ImVec4 kFamModel { 0.82f, 0.41f, 0.12f, 1.0f }; ///< @brief h= 25 形状
static constexpr ImVec4 kFamAnim  { 0.76f, 0.88f, 0.18f, 1.0f }; ///< @brief h= 70 時間軸を持つもの
static constexpr ImVec4 kFamCode  { 0.20f, 0.70f, 0.36f, 1.0f }; ///< @brief h=140 コード / ロジック
static constexpr ImVec4 kFamFont  { 0.33f, 0.33f, 0.88f, 1.0f }; ///< @brief h=240 UI / フォント
static constexpr ImVec4 kFamScene { 0.63f, 0.24f, 0.80f, 1.0f }; ///< @brief h=282 シーン / プレファブ
static constexpr ImVec4 kFamAudio { 0.72f, 0.18f, 0.52f, 1.0f }; ///< @brief h=322 音
static constexpr ImVec4 kFamVfx   { 0.98f, 0.37f, 0.47f, 1.0f }; ///< @brief h=350 エフェクト
static constexpr ImVec4 kFamData  { 0.58f, 0.58f, 0.58f, 1.0f }; ///< @brief 無彩色 データ / テキスト

static constexpr ExtGroup kExtGroups[] = {
    /// @name Look
    { { ".png", ".jpg", ".jpeg", ".dds", ".bmp", ".tga" },     kFamLook,  "TEX"       },
    { { ".mat", nullptr },                                     kFamLook,  "MAT"       },
    /// @note 物理マテリアルも「マテリアル」の一員。ラベルで見分ける。
    { { ".physmat", nullptr },                                 kFamLook,  "PHYSMAT"   },
    { { ".tex", nullptr },                                     kFamLook,  "TEXDESC"   },
    /// @note .ico は «アプリのアイコン» という役割で、素材テクスチャとは用途が違う。
    { { ".ico", nullptr },                                     kFamLook,  "ICON"      },

    /// @name Model
    { { ".fbx", ".obj", ".gltf", ".glb", nullptr },            kFamModel, "MESH"      },
    { { ".mesh", nullptr },                                    kFamModel, "MESH"      },
    { { ".terrain", nullptr },                                 kFamModel, "TERRAIN"   },

    /// @name Animation (時間軸を持つもの)
    { { ".anim", nullptr },                                    kFamAnim,  "ANIM"      },
    { { ".animcontroller", nullptr },                          kFamAnim,  "ANIM CTRL" },
    { { ".animctrl", nullptr },                                kFamAnim,  "CTRL"      },
    { { ".mask", nullptr },                                    kFamAnim,  "MASK"      },
    { { ".sequence", nullptr },                                kFamAnim,  "SEQ"       },

    /// @name Code & Logic
    { { ".hlsl", ".hlsli", nullptr },                          kFamCode,  "HLSL"      },
    { { ".hpp", ".cpp", ".h", ".c", ".cc", ".cxx" },           kFamCode,  "CPP"       },
    { { ".py", ".lua", ".cs", nullptr },                       kFamCode,  "SCRIPT"    },
    { { ".behaviortree", nullptr },                            kFamCode,  "AI"        },

    /// @name UI & Font
    { { ".ttf", ".ttc", ".otf", nullptr },                     kFamFont,  "FONT"      },
    { { ".fnt", nullptr },                                     kFamFont,  "FNT"       },

    /// @name Scene & Prefab
    { { ".scene", nullptr },                                   kFamScene, "SCENE"     },
    { { ".prefab", nullptr },                                  kFamScene, "PREFAB"    },

    /// @name Audio
    { { ".wav", ".mp3", ".ogg", ".flac", nullptr },            kFamAudio, "SFX"       },
    { { ".synth", nullptr },                                   kFamAudio, "SYNTH"     },

    /// @name VFX
    { { ".vfx", nullptr },                                     kFamVfx,   "VFX"       },
    { { ".fga", nullptr },                          kFamVfx,   "FGA"    },
    { { ".fluid", nullptr },                                   kFamVfx,   "FLUID"     },
    /// @note 曲線と色は «時間軸を持つもの» の一員。エフェクト以外でも使い回すので Anim 側に置く。
    { { ".curve", nullptr },                                   kFamAnim,  "CURVE"     },
    { { ".gradient", nullptr },                                kFamLook,  "GRADIENT"  },

    /// @name Data & Text
    { { ".toml", ".json", ".yaml", ".yml", nullptr },          kFamData,  "DATA"      },
    { { ".txt", ".md", ".rst", nullptr },                      kFamData,  "TEXT"      },
    { { ".asset", nullptr },                                   kFamData,  "ASSET"     },
};

/// @brief 未知拡張子をハッシュで色付けする。
/// @note FNV-1a の下位ビットを色相に変換し、彩度・明度は固定で読みやすい明るさに調整する。
///       同じ拡張子なら常に同じ色になる。
static ImVec4 ColorFromExt(const std::string& ext)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : ext)
        h = (h ^ c) * 16777619u;
    /// @note 0..1
    const float hue = static_cast<float>(h & 0xFFFF) / 65536.0f;
    /// @note HSV → RGB (S=0.55, V=0.72)
    const float s = 0.55f, v = 0.72f;
    const float hi = std::fmodf(hue * 6.0f, 6.0f);
    const int   i  = static_cast<int>(hi);
    const float f  = hi - static_cast<float>(i);
    const float p  = v * (1.0f - s);
    const float q  = v * (1.0f - s * f);
    const float t  = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
    case 0: return { v, t, p, 1.0f };
    case 1: return { q, v, p, 1.0f };
    case 2: return { p, v, t, 1.0f };
    case 3: return { p, q, v, 1.0f };
    case 4: return { t, p, v, 1.0f };
    default:return { v, p, q, 1.0f };
    }
}

/// @brief 拡張子がグループに含まれるか確認する。
static const ExtGroup* FindGroup(const std::string& ext)
{
    for (const auto& g : kExtGroups) {
        for (int i = 0; i < 6 && g.exts[i]; ++i)
            if (ext == g.exts[i]) return &g;
    }
    return nullptr;
}

static bool IsTextureExt(const std::string& ext)
{
    /// @note .dds はキューブマップ等の非 2D テクスチャを含むため 2D プレビュー対象から除外する
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".bmp" || ext == ".tga" || ext == ".ico";
}

static bool IsMeshExt(const std::string& ext)
{
    return ext == ".fbx" || ext == ".obj" || ext == ".gltf" ||
           ext == ".glb" || ext == ".mesh";
}

static std::filesystem::file_time_type ReadLastWriteTime(const std::string& path)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(path), ec);
    return ec ? std::filesystem::file_time_type{} : time;
}

static std::string ToTextureLoadPath(const std::string& path, const EditorContext& ctx)
{
    const std::string normalized = util::FileSystem::NormalizePathSeparators(path);
    const std::string projectRoot = util::FileSystem::NormalizePathSeparators(ctx.projectRoot);

    /// @note .mat 内のテクスチャ参照は Assets/ 相対で保存されるため、
    ///       ResourceManager が読める実ファイルパスに変換してからサムネイルを読み込む。
    if (normalized.starts_with("Assets/") && !projectRoot.empty()) {
        return projectRoot + "/" + normalized;
    }
    return normalized;
}

/// @brief FBX の従属アセットを Library/Baked/`<guid>` から解決する。
/// @note インポーターが生成する .fzasset / .mat の正規配置を一箇所に固定し、
///       AssetBrowser が原本 FBX 隣の中間生成物へ依存しないようにする。
static std::filesystem::path ResolveModelGeneratedDir(const std::string& sourcePath,
                                                       const char* generatedName)
{
    const std::string bakedDir = asset::AssetManager::BakedDirForSource(sourcePath);
    if (bakedDir.empty()) return {};
    return util::FileSystem::PathFromUtf8(bakedDir) / generatedName;
}

static ImTextureID ToImTextureID(void* ptr)
{
    /// @note このプロジェクトの ImGui は ImTextureID を ImU64 として扱う。
    ///       void* のビット列を整数 ID に移すだけなので、所有権や型変換の意味を持たせない。
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

/// @brief .ico を GPU テクスチャにする。
/// @note ResourceManager::LoadTexture を通さないのは、下地の DirectXTex (WIC) は .ico を読めるが
///       «先頭フレーム» しか返さないため。.ico は 16px〜256px を束ねた形式で、ファイルによっては
///       16px がサムネイルに出るので、面積最大のフレームを自前で選ぶ。
static renderer::ResourceHandle<renderer::TextureTag> LoadIcoTexture(
    renderer::ResourceManager& resources,
    const std::string& path,
    uint32_t& outWidth,
    uint32_t& outHeight)
{
    IcoImage image;
    std::string error;
    if (!DecodeIcoFile(util::FileSystem::PathFromUtf8(path), image, error) || !image.IsValid()) {
        FBZZ_LOG_WARN("Icon preview failed: %s (%s)", path.c_str(), error.c_str());
        return {};
    }
    outWidth  = static_cast<uint32_t>(image.width);
    outHeight = static_cast<uint32_t>(image.height);
    return resources.CreateTexture(image.rgba.data(), outWidth, outHeight);
}

static std::string SelectMaterialPreviewTexture(const asset::MaterialAsset& mat)
{
    return matpreview::RepresentativeTexturePath(mat);
}

static ImVec4 SelectMaterialColor(const asset::MaterialAsset& mat)
{
    const math::Vector4 color = matpreview::AlbedoColor(mat);
    return { color.x, color.y, color.z, color.w };
}

} // namespace

bool AssetBrowserPanel::RebuildMaterialThumbnailGpuData(MaterialPreview& preview, EditorContext& ctx)
{
    if (!ctx.resources) return false;
    return matpreview::BuildGpuData(preview.gpu, preview.asset, *ctx.resources, ctx.projectRoot);
}

namespace {

static void DrawThumbnailFrame(ImVec2 origin, float sz, bool hovered)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = hovered ? IM_COL32(42, 45, 52, 255) : IM_COL32(30, 32, 38, 255);
    dl->AddRectFilled(origin, { origin.x + sz, origin.y + sz }, bg, 4.0f);
    /// @note 上明→下暗の縦グラデーションで背景に奥行きを持たせ、プレビューの立体感を補強する。
    ///       AddRectFilledMultiColor は角丸非対応のため、角丸 (4px) の欠けが届かない
    ///       2px 内側へ矩形で重ね、ベースの角丸輪郭を保つ。
    const ImU32 gradTop    = hovered ? IM_COL32(56, 60, 70, 255) : IM_COL32(44, 47, 56, 255);
    const ImU32 gradBottom = hovered ? IM_COL32(30, 32, 38, 255) : IM_COL32(19, 20, 24, 255);
    dl->AddRectFilledMultiColor({ origin.x + 2.0f, origin.y + 2.0f },
                                { origin.x + sz - 2.0f, origin.y + sz - 2.0f },
                                gradTop, gradTop, gradBottom, gradBottom);
    dl->AddRect(origin, { origin.x + sz, origin.y + sz }, IM_COL32(95, 100, 112, 230), 4.0f, 0, 1.0f);
}

static void DrawTextureThumbnail(void* rawID, uint32_t width, uint32_t height, ImVec2 origin, float sz, bool hovered)
{
    DrawThumbnailFrame(origin, sz, hovered);
    const float frameH = sz;
    const float w = static_cast<float>(std::max<uint32_t>(1, width));
    const float h = static_cast<float>(std::max<uint32_t>(1, height));
    const float scale = std::min((sz - 8.0f) / w, (frameH - 8.0f) / h);
    const ImVec2 imageSize = { std::max(1.0f, w * scale), std::max(1.0f, h * scale) };
    const ImVec2 imageMin = {
        origin.x + (sz - imageSize.x) * 0.5f,
        origin.y + (frameH - imageSize.y) * 0.5f
    };
    ImGui::GetWindowDrawList()->AddImage(
        ToImTextureID(rawID),
        imageMin,
        { imageMin.x + imageSize.x, imageMin.y + imageSize.y });
}

/// @brief Sprite Texture の元素材は atlas 全体、仮想サブアセットは個別 SpriteRect を切り抜いて表示する。
/// @note 元画像と個々の Sprite の見た目を同時に比較できる Unity 風の展開表示にする。
static void DrawSpriteThumbnail(void* rawID, uint32_t width, uint32_t height,
                                const asset::SpriteRect* sprite,
                                const char* badge,
                                ImVec2 origin, float sz, bool hovered)
{
    DrawThumbnailFrame(origin, sz, hovered);
    const float textureWidth = static_cast<float>(std::max<uint32_t>(1, width));
    const float textureHeight = static_cast<float>(std::max<uint32_t>(1, height));
    ImVec2 uvMin = { 0.0f, 0.0f };
    ImVec2 uvMax = { 1.0f, 1.0f };
    float spriteWidth = textureWidth;
    float spriteHeight = textureHeight;

    if (sprite) {
        spriteWidth = sprite->width > 0 ? static_cast<float>(sprite->width) : textureWidth;
        spriteHeight = sprite->height > 0 ? static_cast<float>(sprite->height) : textureHeight;
        uvMin = {
            std::clamp(static_cast<float>(sprite->x) / textureWidth, 0.0f, 1.0f),
            std::clamp(static_cast<float>(sprite->y) / textureHeight, 0.0f, 1.0f)
        };
        uvMax = {
            std::clamp((static_cast<float>(sprite->x) + spriteWidth) / textureWidth, 0.0f, 1.0f),
            std::clamp((static_cast<float>(sprite->y) + spriteHeight) / textureHeight, 0.0f, 1.0f)
        };
    }

    const float scale = std::min((sz - 8.0f) / std::max(1.0f, spriteWidth),
                                 (sz - 8.0f) / std::max(1.0f, spriteHeight));
    const ImVec2 imageSize = {
        std::max(1.0f, spriteWidth * scale),
        std::max(1.0f, spriteHeight * scale)
    };
    const ImVec2 imageMin = {
        origin.x + (sz - imageSize.x) * 0.5f,
        origin.y + (sz - imageSize.y) * 0.5f
    };
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddImage(ToImTextureID(rawID), imageMin,
                       { imageMin.x + imageSize.x, imageMin.y + imageSize.y },
                       uvMin, uvMax);

    if (!badge || badge[0] == '\0') return;
    const ImVec2 textSize = ImGui::CalcTextSize(badge);
    const ImVec2 badgeMin = { origin.x + 4.0f, origin.y + 4.0f };
    const ImVec2 badgeMax = { badgeMin.x + textSize.x + 8.0f, badgeMin.y + textSize.y + 4.0f };
    drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(20, 26, 22, 220), 3.0f);
    drawList->AddText({ badgeMin.x + 4.0f, badgeMin.y + 2.0f },
                       IM_COL32(105, 235, 135, 255), badge);
}

/// @brief メッシュ 1 つを正方形 RT へ焼く。
/// @note 実際の描画は MaterialPreviewCore が持つ。ここは AssetBrowser の «このメッシュをこの色で»
///       という呼び出し形をそのまま受けるだけの薄い口で、サムネイルと Inspector / Preview パネルが
///       同じ照明・同じカメラで焼かれる。
static bool RenderMeshThumbnail(
    renderer::IRenderer& renderer,
    renderer::ResourceManager& resources,
    const renderer::Mesh& mesh,
    renderer::ResourceHandle<renderer::RenderTargetTag> rt,
    renderer::ResourceHandle<renderer::TextureTag> albedoTexture,
    ImVec4 albedoColor,
    const matpreview::GpuData* materialGpu = nullptr,
    matpreview::Flavor flavor = matpreview::Flavor::Surface,
    const asset::MaterialAsset* materialAsset = nullptr,
    bool clearRT = true,
    math::Vector3 overrideCenter = {},
    float overrideRadius = -1.0f)  ///< @brief <0 = use mesh bounds
{
    matpreview::RenderDesc desc;
    desc.target          = rt;
    desc.mesh            = &mesh;
    desc.flavor          = flavor;
    desc.material        = materialAsset;
    desc.gpu             = materialGpu;
    desc.fallbackTexture = albedoTexture;
    desc.fallbackColor   = { albedoColor.x, albedoColor.y, albedoColor.z, albedoColor.w };
    desc.clear           = clearRT;
    desc.overrideCenter  = overrideCenter;
    desc.overrideRadius  = overrideRadius;
    return matpreview::Render(renderer, resources, desc);
}

static void DrawThumbnailLabel(ImDrawList* dl, ImVec2 origin, float sz, const char* badge)
{
    const ImVec2 tsz     = ImGui::CalcTextSize(badge);
    const ImVec2 bMin    = { origin.x + sz - tsz.x - 12.0f, origin.y + sz - tsz.y - 7.0f };
    const ImVec2 bMax    = { origin.x + sz - 4.0f,           origin.y + sz - 3.0f         };
    dl->AddRectFilled(bMin, bMax, IM_COL32(20, 22, 26, 205), 3.0f);
    dl->AddText({ bMin.x + 4.0f, bMin.y + 2.0f }, IM_COL32(235, 240, 245, 230), badge);
}

/// @brief 球に焼けない .mat の種別バッジと色見本。判定も色選びも MaterialPreviewCore と共有する。
static const char* MaterialThumbnailBadge(const asset::MaterialAsset& asset)
{
    return matpreview::UnsupportedBadge(asset);
}

static ImVec4 SelectSwatchColor(const asset::MaterialAsset& asset)
{
    const math::Vector4 color = matpreview::SwatchColor(asset);
    return { color.x, color.y, color.z, color.w };
}

/// @brief 3D に焼けない .mat の最後の受け皿。色と種別だけでも出して、拡張子アイコンに落とさない。
static void DrawMaterialSwatchThumbnail(const asset::MaterialAsset& asset, const char* badge,
                                        ImVec2 origin, float sz, bool hovered)
{
    DrawThumbnailFrame(origin, sz, hovered);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec4 color = SelectSwatchColor(asset);
    const ImU32  top    = ImGui::ColorConvertFloat4ToU32(color);
    const ImU32  bottom = ImGui::ColorConvertFloat4ToU32(
        { color.x * 0.35f, color.y * 0.35f, color.z * 0.35f, 1.0f });

    const float inset = sz * 0.18f;
    const ImVec2 chipMin = { origin.x + inset,      origin.y + inset };
    const ImVec2 chipMax = { origin.x + sz - inset, origin.y + sz - inset };
    dl->AddRectFilledMultiColor(chipMin, chipMax, top, top, bottom, bottom);
    dl->AddRect(chipMin, chipMax, IM_COL32(20, 22, 26, 180), 0.0f, 0, 1.0f);
    DrawThumbnailLabel(dl, origin, sz, badge);
}

static void DrawRenderTargetThumbnail(
    renderer::ResourceHandle<renderer::RenderTargetTag> rt,
    ImVec2 origin,
    float sz,
    EditorContext& ctx,
    bool hovered,
    const char* badge)
{
    DrawThumbnailFrame(origin, sz, hovered);
    if (!ctx.imguiRenderer || !ctx.resources || !rt.IsValid()) return;

    void* rawID = ctx.imguiRenderer->GetImTextureID(rt, *ctx.resources, 0);
    if (!rawID) return;

    const float frameH = sz;
    const float margin = std::max(4.0f, sz * 0.06f);
    ImGui::GetWindowDrawList()->AddImage(
        ToImTextureID(rawID),
        { origin.x + margin, origin.y + margin },
        { origin.x + sz - margin, origin.y + frameH - margin });

    const ImVec2 textSize = ImGui::CalcTextSize(badge);
    const ImVec2 badgeMin = { origin.x + sz - textSize.x - 12.0f, origin.y + frameH - textSize.y - 7.0f };
    const ImVec2 badgeMax = { origin.x + sz - 4.0f, origin.y + frameH - 3.0f };
    ImGui::GetWindowDrawList()->AddRectFilled(badgeMin, badgeMax, IM_COL32(20, 22, 26, 205), 3.0f);
    ImGui::GetWindowDrawList()->AddText({ badgeMin.x + 4.0f, badgeMin.y + 2.0f },
                                        IM_COL32(235, 240, 245, 230), badge);
}

/// @name プレビュー失敗の再試行
/// @brief 一括投入直後の失敗はたいてい「まだ書き込みが終わっていない」だけで、数百 ms 後に成功する。
/// @brief 1 回で打ち切ると再起動まで直らず、無限に試すと壊れた素材で毎フレーム Assimp / WIC を回す。
constexpr uint32_t kPreviewMaxRetries    = 10;
constexpr double   kPreviewRetryInterval = 0.5;

template<typename T>
static bool CanAttemptPreview(const T& p) {
    if (!p.failed) return true;
    return p.retry.count < kPreviewMaxRetries && ImGui::GetTime() >= p.retry.nextTime;
}

template<typename T>
static void MarkPreviewFailed(T& p) {
    p.failed = true;
    p.retry.nextTime = ImGui::GetTime() + kPreviewRetryInterval;
    ++p.retry.count;
}

template<typename T>
static void MarkPreviewSucceeded(T& p) {
    p.failed = false;
    p.retry  = {};
}

template<typename T>
static void EnsureThumbnailRT(T& t, EditorContext& ctx) {
    if (!t.thumbnailRT.IsValid()) {
        t.thumbnailRT = ctx.resources->CreateRenderTarget(128, 128);
        t.thumbnailRendered = false;
    }
}

/// @brief マテリアルサムネイルの GPU 側キャッシュ (シェーダー / CB / テクスチャ) を捨て、
/// @brief 次フレームで RebuildMaterialThumbnailGpuData から作り直させる。
/// @brief .mat の再読み込み経路がディスク更新と Inspector 編集の 2 つあるので 1 箇所にまとめる。
template<typename T>
static void ResetMaterialPreviewGpuState(T& preview, renderer::ResourceManager* resources) {
    preview.previewTexture = {};
    preview.previewTexturePath.clear();
    preview.previewTextureWidth = 0;
    preview.previewTextureHeight = 0;
    matpreview::ResetGpuData(preview.gpu, resources);
    preview.thumbnailRendered = false;
}

/// @brief Returns true if the thumbnail was drawn; caller should `return` immediately.
template<typename T>
static bool DrawThumbnailIfReady(T& t, ImVec2 origin, float sz,
                                  EditorContext& ctx, bool hovered, const char* badge) {
    if (!t.thumbnailRendered) return false;
    DrawRenderTargetThumbnail(t.thumbnailRT, origin, sz, ctx, hovered, badge);
    return true;
}

} // namespace

void AssetBrowserPanel::QueueAssetMove(const std::string& payloadPath, const std::string& targetDir)
{
    if (payloadPath.empty() || targetDir.empty() || m_pendingAssetMove.active) return;

    m_pendingAssetMove.sourcePaths = dragdrop::DraggedAssetPaths(payloadPath);
    m_pendingAssetMove.targetDir   = targetDir;
    m_pendingAssetMove.active      = true;
}

void AssetBrowserPanel::PublishAssetDrag(const Entry& e, EditorContext& ctx)
{
    const std::string payloadPath = ToAssetDragPayloadPath(e.path, ctx);
    ImGui::SetDragDropPayload("ASSET_PATH", payloadPath.c_str(), payloadPath.size() + 1);

    /// @note 見た目で選択されているもの (主選択 + 追加選択) を運ぶ。描画側の selected と同じ規則。
    const bool grabbedSelected =
        (!e.isDir && e.path == ctx.selectedAssetPath) || m_selectedPaths.count(e.path) > 0;
    std::vector<std::string> paths;
    if (grabbedSelected) {
        paths.reserve(m_selectedPaths.size() + 1);
        /// @note フォルダの主選択は選択として描かれないので運ばない。
        if (!ctx.selectedAssetPath.empty() && !util::FileSystem::IsDirectory(ctx.selectedAssetPath))
            paths.push_back(ToAssetDragPayloadPath(ctx.selectedAssetPath, ctx));
        for (const std::string& path : m_selectedPaths)
            paths.push_back(ToAssetDragPayloadPath(path, ctx));
    }
    dragdrop::SetAssetDrag(payloadPath, std::move(paths));

    const size_t count = dragdrop::DraggedAssetPaths(payloadPath).size();
    if (count > 1) ImGui::Text("%s (+%d)", e.name.c_str(), static_cast<int>(count - 1));
    else           ImGui::TextUnformatted(e.name.c_str());
}

void AssetBrowserPanel::FinalizePendingAssetMove(EditorContext& ctx)
{
    if (!m_pendingAssetMove.active) return;

    /// @note 先にキューを空にする。失敗時も同じ payload が次フレームに残らないようにする。
    const PendingAssetMove request = std::move(m_pendingAssetMove);
    m_pendingAssetMove = {};

    std::string lastMoved;
    int         movedCount = 0;
    for (const std::string& sourcePath : request.sourcePaths) {
        std::string srcAbs;
        std::string dstAbs;
        /// @note 移動先フォルダ自身は運ばれていても動かさない (自分の中へは入れられない)。
        if (util::FileSystem::SamePathText(ToAssetDragPayloadPath(request.targetDir, ctx), sourcePath))
            continue;
        if (!MoveProjectAssetToDirectory(sourcePath, request.targetDir, ctx, srcAbs, dstAbs)) {
            Toast::Error("Move failed: " + sourcePath);
            continue;
        }
        if (util::FileSystem::SamePathText(ctx.selectedAssetPath, srcAbs))
            ClearAssetSelection(ctx);
        /// @note 追加選択に古いパスを残すと、次の操作が存在しないファイルを指す。
        for (auto it = m_selectedPaths.begin(); it != m_selectedPaths.end();) {
            if (util::FileSystem::SamePathText(*it, srcAbs)) it = m_selectedPaths.erase(it);
            else                                             ++it;
        }
        ResetAssetPreviewCache(srcAbs);
        InvalidateTreeCache(util::FileSystem::GetDirectory(srcAbs));
        lastMoved = dstAbs;
        ++movedCount;
    }
    if (movedCount == 0) return;
    InvalidateTreeCache(request.targetDir);

    /// @note ここは全アイテムの描画が終わった後なので、m_entries を安全に再構築できる。
    RefreshDirectory();
    ctx.requestAssetBrowserRefresh = false;
    if (movedCount == 1) Toast::Success("Moved " + util::FileSystem::GetFilename(lastMoved));
    else                 Toast::Success("Moved " + std::to_string(movedCount) + " items");
}

const char* AssetBrowserPanel::TypeFilterLabel(TypeFilter type)
{
    using TF = TypeFilter;
    switch (type) {
    case TF::Scene:     return "Scene";
    case TF::Material:  return "Material";
    case TF::Script:    return "Script";
    case TF::Texture:   return "Texture";
    case TF::Audio:     return "Audio";
    case TF::Mesh:      return "Mesh";
    case TF::Shader:    return "Shader";
    case TF::Prefab:    return "Prefab";
    case TF::Animation: return "Animation";
    case TF::Asset:     return "Asset";
    default:            return "All";
    }
}

bool AssetBrowserPanel::TryGetFolderColor(const EditorContext& ctx,
                                          const std::string& folderPath,
                                          ImVec4& outColor)
{
    const auto it = ctx.assetBrowserFolderColors.find(
        util::FileSystem::NormalizePathSeparators(folderPath));
    if (it == ctx.assetBrowserFolderColors.end()) return false;
    outColor = ImGui::ColorConvertU32ToFloat4(it->second);
    return true;
}

ImVec4 AssetBrowserPanel::ResolveEntryColor(const Entry& e, const EditorContext& ctx) const
{
    ImVec4 folderColor;
    if (e.isDir && TryGetFolderColor(ctx, e.path, folderColor)) return folderColor;
    return EntryColor(e);
}

void AssetBrowserPanel::ApplyFolderColor(const std::string& folderPath,
                                         const uint32_t* color,
                                         EditorContext& ctx)
{
    const std::string key = util::FileSystem::NormalizePathSeparators(folderPath);
    if (color) ctx.assetBrowserFolderColors[key] = *color;
    else       ctx.assetBrowserFolderColors.erase(key);

    if (!m_folderColorApplyRecursive) return;

    for (const auto& child : util::FileSystem::ListAll(key)) {
        if (!util::FileSystem::IsDirectory(child)) continue;
        const std::string childPath = util::FileSystem::NormalizePathSeparators(child);
        if (!ShouldDisplayEntry(childPath, util::FileSystem::GetFilename(child), true)) continue;
        ApplyFolderColor(childPath, color, ctx);
    }
}

void AssetBrowserPanel::PushRecentFolderColor(uint32_t color, EditorContext& ctx)
{
    auto& recent = ctx.assetBrowserRecentFolderColors;
    recent.erase(std::remove(recent.begin(), recent.end(), color), recent.end());
    recent.insert(recent.begin(), color);
    if (recent.size() > kMaxRecentFolderColors) recent.resize(kMaxRecentFolderColors);
}

void AssetBrowserPanel::DrawFolderColorMenu(const std::string& folderPath, EditorContext& ctx)
{
    /// @note 彩度を抑えた 8 色。フォルダの識別が目的なので、アセット種別の色と
    ///       competing しない程度の明度に揃える (タイル一面が原色になると帯が読めない)。
    static constexpr ImVec4 kPresets[] = {
        { 0.86f, 0.30f, 0.30f, 1.0f }, { 0.90f, 0.55f, 0.20f, 1.0f },
        { 0.88f, 0.80f, 0.25f, 1.0f }, { 0.40f, 0.78f, 0.38f, 1.0f },
        { 0.28f, 0.72f, 0.72f, 1.0f }, { 0.32f, 0.56f, 0.90f, 1.0f },
        { 0.62f, 0.42f, 0.88f, 1.0f }, { 0.88f, 0.45f, 0.72f, 1.0f },
    };
    constexpr ImGuiColorEditFlags kSwatchFlags =
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha;

    /// @note 現在色はイテレータではなく値で持つ。このメニューの中で色を確定すると unordered_map
    ///       へ挿入が起き、保持していたイテレータが無効化される (以降の参照が未定義動作になる)。
    const std::string key = util::FileSystem::NormalizePathSeparators(folderPath);
    const auto found = ctx.assetBrowserFolderColors.find(key);
    const bool     hasColor     = found != ctx.assetBrowserFolderColors.end();
    const uint32_t currentColor = hasColor ? found->second : 0u;

    /// @note 設定済みかどうかはラベル自体で示す。色見本をラベルの左に描かないのは、メニュー項目は
    ///       行幅いっぱいに広がるため、SameLine で図形を差し込むと当たり判定と表示がずれるため。
    if (!ImGui::BeginMenu(hasColor ? "Set Color \xe2\x97\x8f" : "Set Color")) return;

    /// @note 色を確定する共通経路。最近使った色への記録と再描画の後始末をここに集約する。
    const auto commit = [&](const ImVec4& picked) {
        const uint32_t packed = ImGui::ColorConvertFloat4ToU32(picked);
        ApplyFolderColor(folderPath, &packed, ctx);
        PushRecentFolderColor(packed, ctx);
    };

    const float swatch = ImGui::GetFrameHeight();

    /// @note 現在の色。設定済みのフォルダで「今どれが効いているのか」を最初に見せる。
    if (hasColor) {
        ImGui::TextDisabled("Current");
        ImGui::ColorButton("##current", ImGui::ColorConvertU32ToFloat4(currentColor),
                           kSwatchFlags,
                           { swatch * 4.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f, swatch });
        ImGui::Separator();
    }

    ImGui::TextDisabled("Presets");
    for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
        ImGui::PushID(i);
        if (ImGui::ColorButton("##preset", kPresets[i], kSwatchFlags, { swatch, swatch })) {
            commit(kPresets[i]);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
        if (i % 4 != 3) ImGui::SameLine();
    }

    if (!ctx.assetBrowserRecentFolderColors.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Recent");
        for (std::size_t i = 0; i < ctx.assetBrowserRecentFolderColors.size(); ++i) {
            const ImVec4 color = ImGui::ColorConvertU32ToFloat4(
                ctx.assetBrowserRecentFolderColors[i]);
            ImGui::PushID(static_cast<int>(i) + 1000);
            if (ImGui::ColorButton("##recent", color, kSwatchFlags, { swatch, swatch })) {
                commit(color);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
            if (i % 4 != 3 && i + 1 < ctx.assetBrowserRecentFolderColors.size())
                ImGui::SameLine();
        }
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Custom...")) {
        /// @note 対象が変わったら、そのフォルダの現在色 (未設定なら既定のフォルダ色) から編集を始める。
        if (m_folderColorPickerPath != key) {
            m_folderColorPickerPath  = key;
            m_folderColorPickerValue = hasColor
                ? ImGui::ColorConvertU32ToFloat4(currentColor)
                : ImVec4{ 0.80f, 0.60f, 0.10f, 1.0f };
        }
        /// @note ピッカーの操作中は都度適用する。決定してからでないと結果が見えないと、
        ///       ツリーやタイルの中でその色がどう見えるか分からないまま選ぶことになる。
        if (ImGui::ColorPicker3("##custom", &m_folderColorPickerValue.x,
                                ImGuiColorEditFlags_NoSidePreview |
                                ImGuiColorEditFlags_NoSmallPreview |
                                ImGuiColorEditFlags_DisplayHex)) {
            const uint32_t packed = ImGui::ColorConvertFloat4ToU32(m_folderColorPickerValue);
            ApplyFolderColor(folderPath, &packed, ctx);
        }
        if (ImGui::Button("Apply", { -FLT_MIN, 0.0f })) {
            commit(m_folderColorPickerValue);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }

    ImGui::Checkbox("Apply to Subfolders", &m_folderColorApplyRecursive);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("このフォルダ配下のフォルダにも同じ色 / 解除を適用します");

    ImGui::Separator();
    if (ImGui::MenuItem("Reset to Default", nullptr, false,
                        hasColor || m_folderColorApplyRecursive)) {
        ApplyFolderColor(folderPath, nullptr, ctx);
    }

    ImGui::EndMenu();
}

ImVec4 AssetBrowserPanel::EntryColor(const Entry& e)
{
    if (e.isDir) return { 0.80f, 0.60f, 0.10f, 1.0f };
    if (const ExtGroup* g = FindGroup(e.ext)) return g->color;
    if (e.ext.empty()) return { 0.38f, 0.38f, 0.38f, 1.0f };
    /// @note 未知拡張子: ハッシュで自動着色
    return ColorFromExt(e.ext);
}

const char* AssetBrowserPanel::EntryLabel(const Entry& e)
{
    if (e.isDir) return "DIR";
    if (const ExtGroup* g = FindGroup(e.ext)) return g->label;
    /// @note 未知拡張子: 拡張子文字列をそのままラベルに使う (最大 6 文字、先頭の . を除く)。
    ///       静的バッファに詰めることでどんな拡張子でもラベル表示でき、ImGui はフレーム内で
    ///       文字列を参照するため static thread_local を使う。
    static thread_local char buf[8];
    /// @note skip '.'
    const char* src = e.ext.size() > 1 ? e.ext.c_str() + 1 : e.ext.c_str();
    const std::string upper = util::StringUtils::ToUpper(src);
    const std::size_t len = std::min<std::size_t>(upper.size(), 6);
    std::memcpy(buf, upper.data(), len);
    buf[len] = '\0';
    return len > 0 ? buf : "FILE";
}

/// @name フォルダツリー (左ペイン)

void AssetBrowserPanel::DrawFolderTree(const std::string& dirPath, EditorContext& ctx)
{
    const std::string normDir = util::FileSystem::NormalizePathSeparators(dirPath);
    auto it = m_treeCache.find(normDir);
    if (it == m_treeCache.end()) {
        /// @note フォルダとファイルの両方を積む。ファイルを描くかどうかは m_treeShowFiles が決めるが、
        ///       キャッシュには常に入れておく。トグルのたびに捨てると開き直すたびに再走査が走るが、
        ///       走査するのは «展開済みのフォルダ» だけなので持っておく方が安い。
        std::vector<Entry> newEntries;
        for (const auto& p : util::FileSystem::ListAll(normDir)) {
            Entry e;
            e.path  = util::FileSystem::NormalizePathSeparators(p);
            e.name  = util::FileSystem::GetFilename(p);
            e.isDir = util::FileSystem::IsDirectory(p);
            if (!ShouldDisplayEntry(e.path, e.name, e.isDir)) continue;
            if (!e.isDir)
                e.ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(e.path));
            newEntries.push_back(std::move(e));
        }
        /// @note マウント (外部フォルダ) は Assets ツリーには混ぜず、左ペインの "EXTERNAL"
        ///       セクション (OnRenderContent) で専用に列挙する。ここでは実フォルダのみ扱う。
        ///       フォルダを先に、その中で名前順。エクスプローラーと同じ並びにする。
        std::stable_sort(newEntries.begin(), newEntries.end(),
                         [](const Entry& a, const Entry& b) {
            if (a.isDir != b.isDir) return a.isDir;
            return a.name < b.name;
        });
        it = m_treeCache.emplace(normDir, std::move(newEntries)).first;
    }
    /// @note 参照ではなくコピーを取る。再帰 DrawFolderTree / RefreshDirectory() が m_treeCache に
    ///       insert/erase すると unordered_map のリハッシュや対象エントリ削除で参照が無効化 (UB) される。
    const std::vector<Entry> dirs = it->second;

    for (const Entry& dir : dirs) {
        /// @note ファイルはフォルダの後ろにまとまっている (キャッシュ構築時にそう並べた)。
        if (!dir.isDir) {
            if (m_treeShowFiles) DrawTreeFileRow(dir, ctx);
            continue;
        }
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_SpanAvailWidth;
        const bool isCurrent = util::FileSystem::SamePathText(dir.path, m_currentPath);
        if (isCurrent) flags |= ImGuiTreeNodeFlags_Selected;

        /// @note 表示名は Assets 側の仮想名、ID は実パスにすることで同名マウントでも ImGui ID が衝突しない。
        ///       現在フォルダはアクセント色の塗りで強調する (既定の薄い選択色より目立たせる)。
        if (isCurrent)
            ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        /// @note 色を設定したフォルダは行名自体をその色で描き、ツリーを畳んだ状態でも
        ///       グリッド側のカードと同じ色で対応が取れるようにする。
        ImVec4 folderColor;
        const bool colored = !isCurrent && TryGetFolderColor(ctx, dir.path, folderColor);
        if (colored) ImGui::PushStyleColor(ImGuiCol_Text, folderColor);
        bool open = ImGui::TreeNodeEx(dir.path.c_str(), flags, "%s", dir.name.c_str());
        if (colored) ImGui::PopStyleColor();
        if (isCurrent)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::GetDragDropPayload()) {
            ui::DrawSelectionBackground(ImGui::GetWindowDrawList(),
                                        ImGui::GetItemRectMin(),
                                        ImGui::GetItemRectMax(),
                                        true,
                                        true,
                                        false,
                                        3.0f);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            m_currentPath = dir.path;
            RefreshDirectory();
        }
        if (ImGui::BeginPopupContextItem()) {
            auto& bks = ctx.assetBrowserBookmarks;
            const bool already = std::find(bks.begin(), bks.end(), dir.path) != bks.end();
            if (ImGui::MenuItem("New Folder")) {
                std::string newDir = dir.path + "/New Folder";
                for (int n = 1; util::FileSystem::Exists(newDir) && n <= 999; ++n)
                    newDir = dir.path + "/New Folder " + std::to_string(n);
                if (!util::FileSystem::Exists(newDir)) {
                    util::FileSystem::EnsureDirectory(newDir);
                    /// @note Undo 履歴には積まない。旧実装の Undo は RemoveAll(newDir) で、
                    ///       作成後にユーザーがそこへ入れたアセットまで巻き添えで消していた。
                    m_currentPath = dir.path;
                    InvalidateTreeCache(dir.path);
                    RefreshDirectory();
                    BeginRenameForPath(newDir, &ctx);
                }
            }
            if (ImGui::MenuItem("Rename")) {
                m_currentPath = normDir;
                RefreshDirectory();
                BeginRenameForPath(dir.path, &ctx);
            }
            if (ImGui::MenuItem("Duplicate")) {
                const std::string dstPath = UniqueDuplicatePath(dir.path, true);
                if (!dstPath.empty() && CopyAssetPath(dir.path, dstPath, true)) {
                    /// @note 複製も Undo 対象外 (削除と同じく、消したいときは Delete から
                    ///       ごみ箱へ送る)。Ctrl+Z でフォルダごと RemoveAll されない。
                    m_currentPath = normDir;
                    InvalidateTreeCache(normDir);
                    RefreshDirectory();
                    BeginRenameForPath(dstPath, &ctx);
                }
            }
            ImGui::Separator();
            DrawFolderColorMenu(dir.path, ctx);
            ImGui::Separator();
            if (!already && ImGui::MenuItem("\xe2\x98\x85 Add to Favorites"))
                bks.push_back(dir.path);
            if (already && ImGui::MenuItem("Remove from Favorites"))
                bks.erase(std::remove(bks.begin(), bks.end(), dir.path), bks.end());
            ImGui::Separator();
            if (ImGui::MenuItem("Reveal in Explorer")) {
                const std::wstring wpath = util::FileSystem::PathFromUtf8(dir.path).wstring();
                const std::wstring args  = L"/select," + wpath;
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            if (ImGui::MenuItem("Copy Path"))
                ImGui::SetClipboardText(dir.path.c_str());
            if (ImGui::MenuItem("Copy Project Path")) {
                const std::string projectPath = ToProjectAssetPath(dir.path, ctx);
                ImGui::SetClipboardText(projectPath.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                const std::string path = dir.path;
                const std::string parent = normDir;
                EditorContext* context = &ctx;
                ModalDialog::OpenConfirm("Delete Folder",
                    "Delete \"" + util::FileSystem::GetFilename(path) + "\" and all contents?\n"
                    "(moved to .fbzz/Trash — not undoable with Ctrl+Z)",
                    [this, path, parent, context]() {
                        if (util::FileSystem::SamePathText(m_currentPath, path) ||
                            util::FileSystem::IsChildPathText(m_currentPath, path))
                            m_currentPath = parent;
                        TrashAssets({ path }, *context);
                        InvalidateTreeCache(parent);
                        RefreshDirectory();
                    });
            }
            ImGui::EndPopup();
        }
        /// @note ヒエラルキーエンティティをフォルダノードにドロップ → そのフォルダへ Prefab 保存
        if (ImGui::BeginDragDropTarget()) {
            if (SaveHierarchyPayloadAsPrefab(
                    ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, dir.path)) {
                RefreshDirectory();
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                std::string sourcePath;
                if (ReadAssetDragPayload(p, sourcePath))
                    QueueAssetMove(sourcePath, dir.path);
            }
            ImGui::EndDragDropTarget();
        }
        if (open) {
            DrawFolderTree(dir.path, ctx);
            ImGui::TreePop();
        }
    }
}

void AssetBrowserPanel::DrawTreeFileRow(const Entry& e, EditorContext& ctx)
{
    /// @note 行ごとに ID を分けるのは、DrawEntryContextMenu は固定文字列 "##entry_ctx" でポップアップを
    ///       引くため、囲まないと同じツリー内の全ファイル行が同一 ID になり、1 行の右クリックで
    ///       全行が開こうとするため。グリッド側 (DrawEntry) も同じ理由でパスを PushID している。
    ImGui::PushID(e.path.c_str());

    const bool selected = e.path == ctx.selectedAssetPath || m_selectedPaths.count(e.path) > 0;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
                             | ImGuiTreeNodeFlags_NoTreePushOnOpen
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;

    /// @note 名前は通常の文字色で描く。種別で色を付けないのは、ツリーで色が意味を持つのは
    ///       フォルダの色分けだけで、ファイルまで着色すると色が 2 つの意味を持って読めなくなるため。
    ImGui::TreeNodeEx(e.path.c_str(), flags, "%s", e.name.c_str());

    const bool hovered = ImGui::IsItemHovered();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_treeRowDragStarted = false;

    /// @note グリッドと同じ payload を出し、ツリーからも参照欄へ直接ドロップできるようにする。
    if (ImGui::BeginDragDropSource()) {
        m_treeRowDragStarted = true;
        PublishAssetDrag(e, ctx);
        ImGui::EndDragDropSource();
    }

    /// @note 選択はここで完結させる。HandleEntryClick を使わないのは、あちらは Shift 範囲選択を
    ///       グリッドの m_entries に対して解決し、再クリックで遅延リネームに入るため。どちらも
    ///       «今グリッドに出ているフォルダ» が前提で、ツリーの行には噛み合わない。
    /// @note ドラッグの判定に IsMouseDragging は使えない (離したフレームでは常に false)。
    ///       複数選択を運んで別の行の上で離すと、その行の単一選択で選択が潰れていた。
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !m_treeRowDragStarted) {
        const bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
        if (ctrl) {
            if (m_selectedPaths.count(e.path)) m_selectedPaths.erase(e.path);
            else                               m_selectedPaths.insert(e.path);
        } else {
            m_selectedPaths.clear();
        }
        SelectAsset(ctx, e.path);
        m_lastClickedPath = e.path;
    }

    /// @note ダブルクリックはグリッドと同じ「開く」。加えて、そのファイルのフォルダへ移動して
    ///       グリッド側の表示も揃える (木とグリッドが別々の場所を指したままにならない)。
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const std::string folder = util::FileSystem::GetDirectory(e.path);
        if (!util::FileSystem::SamePathText(folder, m_currentPath)) {
            m_pendingNavigate = folder;
            m_scrollToPath    = e.path;
        }
        HandleEntryDoubleClick(e, ctx, true);
    }

    if (hovered) ImGui::SetTooltip("%s", e.path.c_str());
    DrawEntryContextMenu(e, ctx);

    ImGui::PopID();
}

/// @name アイコン描画ユーティリティ

void AssetBrowserPanel::DrawFileIconAt(ImVec2 origin, float sz, const Entry& e, bool hovered,
                                       const ImVec4* colorOverride)
{
    const ImVec4 base  = colorOverride ? *colorOverride : EntryColor(e);
    const ImU32 cFill  = ImGui::ColorConvertFloat4ToU32(hovered ? Lighten(base) : base);
    const ImU32 cDark  = ImGui::ColorConvertFloat4ToU32(
        { base.x * 0.50f, base.y * 0.50f, base.z * 0.50f, 1.0f });
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (e.isDir) {
        const float tabW  = sz * 0.48f;
        const float tabH  = sz * 0.14f;
        const float bodyY = origin.y + tabH;
        const float bodyH = sz * 0.82f;
        const float r     = sz * 0.07f;
        dl->AddRectFilled({ origin.x, origin.y }, { origin.x + tabW, bodyY + r }, cFill, r);
        dl->AddRectFilled({ origin.x, bodyY },    { origin.x + sz,   origin.y + bodyH }, cFill, r);
        dl->AddRect(      { origin.x, bodyY },    { origin.x + sz,   origin.y + bodyH }, cDark, r, 0, 1.0f);
    } else {
        DrawFileIconPolygon(dl, origin, sz, cFill, cDark);
        const char* lbl = EntryLabel(e);
        ImFont* font = ImGui::GetFont();
        const float labelSz = ImGui::GetFontSize() * std::max(1.0f, sz / 64.0f);
        const ImVec2 tsz = font->CalcTextSizeA(labelSz, FLT_MAX, 0.0f, lbl);
        dl->AddText(font, labelSz,
                    { origin.x + (sz - tsz.x) * 0.5f, origin.y + sz * 0.85f * 0.52f - tsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 220), lbl);
    }
}

/// @name アイコン1個 (右ペイン)

void AssetBrowserPanel::DrawAssetPreviewIconAt(ImVec2 origin, float sz, const Entry& e, EditorContext& ctx, bool hovered)
{
    if (e.isDir) {
        ImVec4 folderColor;
        const bool colored = TryGetFolderColor(ctx, e.path, folderColor);
        DrawFileIconAt(origin, sz, e, hovered, colored ? &folderColor : nullptr);
        return;
    }

    if ((IsTextureExt(e.ext) || e.isSpriteSubAsset) && ctx.resources && ctx.imguiRenderer) {
        const std::string& texturePath = e.isSpriteSubAsset ? e.sourceAssetPath : e.path;
        TexturePreview& preview = m_texturePreviews[texturePath];
        if (!preview.handle.IsValid() && !preview.queued && CanAttemptPreview(preview)) {
            preview.queued = true;
            m_texLoadQueue.push_back(texturePath);
        }

        if (preview.handle.IsValid()) {
            void* rawID = ctx.imguiRenderer->GetImTextureID(preview.handle, *ctx.resources);
            if (rawID) {
                SpritePreview& spritePreview = m_spritePreviews[texturePath];
                const std::string metaPath = texturePath + ".meta";
                const auto metaWriteTime = ReadLastWriteTime(metaPath);
                if (!spritePreview.loaded || spritePreview.lastWriteTime != metaWriteTime) {
                    spritePreview = {};
                    spritePreview.lastWriteTime = metaWriteTime;
                    asset::TextureAsset textureAsset;
                    asset::TexDescSerializer serializer;
                    if (serializer.Load(metaPath, textureAsset))
                        spritePreview.settings = std::move(textureAsset.settings);
                    spritePreview.loaded = true;
                }
                if (spritePreview.settings.type == asset::TextureType::Sprite) {
                    if (e.isSpriteSubAsset) {
                        const asset::SpriteRect* sprite =
                            e.spriteIndex < spritePreview.settings.sprites.size()
                            ? &spritePreview.settings.sprites[e.spriteIndex]
                            : nullptr;
                        DrawSpriteThumbnail(rawID, preview.width, preview.height,
                                            sprite, nullptr, origin, sz, hovered);
                    } else {
                        const std::string badge = spritePreview.settings.sprites.size() > 1
                            ? "SPRITE x" + std::to_string(spritePreview.settings.sprites.size())
                            : "SPRITE";
                        DrawSpriteThumbnail(rawID, preview.width, preview.height,
                                            nullptr, badge.c_str(), origin, sz, hovered);
                    }
                } else
                    DrawTextureThumbnail(rawID, preview.width, preview.height, origin, sz, hovered);
                return;
            }
        }
    }

    /// @note 画像 + .meta sidecar: ImageImporter 経由でロードして GPU テクスチャを表示
    if (e.ext == ".tex" && ctx.resources && ctx.imguiRenderer) {
        TexDescPreview& preview = m_texDescPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        if (currentWriteTime != preview.lastWriteTime) {
            preview.lastWriteTime = currentWriteTime;
            preview.handle = {};
            preview.width = preview.height = 0;
            /// @note 中身が変わったので失敗と再試行回数をやり直す
            MarkPreviewSucceeded(preview);
        }
        if (!preview.handle.IsValid() && CanAttemptPreview(preview)) {
            /// @note 前回の失敗は AssetManager の cache にも焼き付いている。掃除しないと
            ///       再試行が同じ null を返すだけで、何度やっても復帰しない。
            if (preview.retry.count > 0) asset::AssetManager::FlushFailed();
            preview.handle = asset::AssetManager::Load<asset::TextureAsset>(e.path);
            if (preview.handle.IsValid()) {
                MarkPreviewSucceeded(preview);
                if (const auto* ta = asset::AssetManager::Get(preview.handle)) {
                    if (const auto* tex = ctx.resources->Get(ta->gpuHandle)) {
                        preview.width  = tex->GetWidth();
                        preview.height = tex->GetHeight();
                    }
                }
            } else {
                MarkPreviewFailed(preview);
            }
        }
        if (preview.handle.IsValid()) {
            if (const auto* ta = asset::AssetManager::Get(preview.handle)) {
                void* rawID = ctx.imguiRenderer->GetImTextureID(ta->gpuHandle, *ctx.resources);
                if (rawID) {
                    DrawTextureThumbnail(rawID, preview.width, preview.height, origin, sz, hovered);
                    return;
                }
            }
        }
    }

    if (e.ext == ".mat") {
        MaterialPreview& preview = m_materialPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        /// @note 3 つ目の条件は「取り込み直後にまだ書き終わっていなかった .mat」の救済。
        ///       更新時刻はコピー完了時点で確定してしまうため、mtime 監視だけでは拾えない。
        if (!preview.loaded || currentWriteTime != preview.lastWriteTime ||
            (preview.failed && CanAttemptPreview(preview))) {
            preview.asset = {};
            if (asset::LoadMaterialAssetFromFile(e.path, preview.asset)) MarkPreviewSucceeded(preview);
            else                                                         MarkPreviewFailed(preview);
            preview.loaded = true;
            preview.lastWriteTime = currentWriteTime;
            ResetMaterialPreviewGpuState(preview, m_resources);
        }

        /// @note Inspector で編集中の .mat は、保存を待たずにサムネイルへ反映する。
        ///       ファイル更新時刻だけだとスライダーを動かしている最中の見た目が古いままになるので、
        ///       Inspector が値変更のたびに進めるリビジョンを見る。
        if (!ctx.materialPreviewRevisions.empty()) {
            const std::string relPath = NormalizeAssetPath(e.path);
            const uint64_t revision = ctx.MaterialPreviewRevision(relPath);
            if (revision != 0 && revision != preview.liveRevision) {
                preview.liveRevision = revision;
                if (const auto* live = asset::AssetManager::Get<asset::MaterialAsset>(
                        asset::AssetManager::Load<asset::MaterialAsset>(relPath))) {
                    preview.asset  = *live;
                    MarkPreviewSucceeded(preview);
                    preview.loaded = true;
                    /// @note ここでは GPU リソースを捨てず、再描画フラグを落とすだけ。ドラッグ中は
                    ///       毎フレーム通るので、定数バッファを作り直すと生成/破棄が延々と続く。
                    preview.thumbnailRendered = false;
                }
            }
        }

        /// @note シェーダーファイルが変更された場合もサムネイルをリセットする
        if (preview.loaded && !preview.failed && !preview.asset.shaderPath.empty()) {
            const std::string resolvedShader = preview.asset.shaderPath.empty()
                ? "Assets/Shaders/Material/Surface/Fallback.hlsl"
                : preview.asset.shaderPath;
            const auto shaderWriteTime = ReadLastWriteTime(
                util::FileSystem::NormalizePathSeparators(ctx.projectRoot + "/" + resolvedShader));
            if (shaderWriteTime != preview.shaderLastWriteTime && shaderWriteTime != std::filesystem::file_time_type{}) {
                preview.shaderLastWriteTime = shaderWriteTime;
                ResetMaterialPreviewGpuState(preview, m_resources);
            }
        }

        if (!preview.failed && ctx.renderer && ctx.resources && ctx.imguiRenderer) {
            const std::string previewTexturePath = SelectMaterialPreviewTexture(preview.asset);
            if (previewTexturePath != preview.previewTexturePath) {
                preview.previewTexturePath = previewTexturePath;
                preview.previewTexture = {};
                preview.previewTextureWidth = 0;
                preview.previewTextureHeight = 0;
                preview.thumbnailRendered = false;
            }

            if (!previewTexturePath.empty()) {
                if (!preview.previewTexture.IsValid()) {
                    preview.previewTexture = ctx.resources->LoadTexture(ToTextureLoadPath(previewTexturePath, ctx));
                    if (auto* texture = ctx.resources->Get(preview.previewTexture)) {
                        preview.previewTextureWidth = texture->GetWidth();
                        preview.previewTextureHeight = texture->GetHeight();
                    }
                }
            }

            /// @note 球に焼けない .mat はここで畳む。RT も確保しない (使わないまま 1 枚寝かせる)。
            ///       素材があればその絵、無ければ色見本。«何も出ない» で終わらせない。
            const matpreview::Flavor flavor = matpreview::DetectFlavor(preview.asset);
            if (flavor == matpreview::Flavor::Unsupported) {
                const char* badge = MaterialThumbnailBadge(preview.asset);
                if (preview.previewTexture.IsValid()) {
                    if (void* rawID = ctx.imguiRenderer->GetImTextureID(preview.previewTexture, *ctx.resources)) {
                        DrawSpriteThumbnail(rawID, preview.previewTextureWidth, preview.previewTextureHeight,
                                            nullptr, badge, origin, sz, hovered);
                        return;
                    }
                }
                DrawMaterialSwatchThumbnail(preview.asset, badge, origin, sz, hovered);
                return;
            }

            EnsureThumbnailRT(preview, ctx);
            if (!preview.thumbnailRendered && preview.thumbnailRT.IsValid()) {
                /// @note 形状は MaterialPreviewCore が Flavor に合わせて選ぶ
                ///       (Surface / Skinned は球、Terrain / Water は細分割した平面、
                ///       UI は ortho の矩形なのでメッシュを持たない、草の Fiber は平面)。
                renderer::Mesh* previewMesh = matpreview::ShapeMesh(
                    *ctx.resources, matpreview::ThumbnailShape(preview.asset, flavor), flavor);
                const bool meshReady = previewMesh || matpreview::UsesOwnGeometry(flavor);

                if (meshReady && RebuildMaterialThumbnailGpuData(preview, ctx)) {
                    matpreview::RenderDesc desc;
                    desc.target   = preview.thumbnailRT;
                    desc.mesh     = previewMesh;
                    desc.flavor   = flavor;
                    desc.material = &preview.asset;
                    desc.gpu      = &preview.gpu;
                    desc.fiberMode = matpreview::DefaultFiberMode(preview.asset);
                    preview.thumbnailRendered =
                        matpreview::Render(*ctx.renderer, *ctx.resources, desc);
                }
            }
            if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "MAT")) return;
            /// @note 球へは焼けるはずなのに焼けなかった .mat (シェーダーが壊れている・
            ///       ShaderDescriptor が引けない等) も、色だけは出して拡張子アイコンに落とさない。
            DrawMaterialSwatchThumbnail(preview.asset, "MAT", origin, sz, hovered);
            return;
        }
    }

    /// @note インポート済み .fbx: LOD0 サブメッシュ + パッケージマテリアルを 3D プレビュー。
    ///       FBX を第一級アセットとして扱い、内部 .fzasset コンテナを UI へ露出しない。
    std::string previewModelPath;
    if (e.ext == ".fbx") {
        previewModelPath = e.path;
    }
    if (!previewModelPath.empty() && ctx.renderer && ctx.resources && ctx.imguiRenderer) {
        ModelAssetPreview& preview = m_modelAssetPreviews[previewModelPath];
        std::string previewWritePath = previewModelPath;
        if (e.ext == ".fbx") {
            const std::filesystem::path p = util::FileSystem::PathFromUtf8(e.path);
            const std::string stem = util::FileSystem::PathToUtf8(p.stem());
            const std::string fbxGuid = asset::AssetDatabase::TryGetGuidFromPath(
                util::FileSystem::NormalizePathSeparators(e.path));
            if (!fbxGuid.empty()) {
                const std::filesystem::path bakedPath =
                    util::FileSystem::PathFromUtf8(ctx.projectRoot)
                    / "Library" / "Baked" / fbxGuid / (stem + ".fzasset");
                const std::string resolved = util::FileSystem::NormalizePathSeparators(
                    util::FileSystem::PathToUtf8(bakedPath));
                if (util::FileSystem::Exists(resolved))
                    previewWritePath = resolved;
            }
        }
        /// @note 物理実体は Library/Baked の内部コンテナなので、更新検知は解決後のパスで行う。
        const auto currentWriteTime = ReadLastWriteTime(previewWritePath);
        if (currentWriteTime != preview.lastWriteTime) {
            if (m_resources)
                for (auto& slot : preview.slotMaterials)
                    matpreview::ResetGpuData(slot.gpu, m_resources);
            preview.slotMaterials.clear();
            preview.materialsLoaded = false;
            preview.lastWriteTime = currentWriteTime;
            preview.handle = {};
            preview.thumbnailRendered = false;
            MarkPreviewSucceeded(preview);
        }
        EnsureThumbnailRT(preview, ctx);
        if (!preview.handle.IsValid() && CanAttemptPreview(preview)) {
            FBZZ_LOG_INFO("AssetBrowserItems: Load<ModelAsset> [%s]", previewModelPath.c_str());
            /// @note インポート直後やファイル監視直後は、生成前に一度 Load して Null が
            ///       AssetManager にキャッシュされることがある。サムネイル再試行時は失敗 cache を掃除する。
            asset::AssetManager::FlushFailed();
            preview.handle = asset::AssetManager::Load<asset::ModelAsset>(previewModelPath);
            if (preview.handle.IsValid()) {
                MarkPreviewSucceeded(preview);
            } else {
                FBZZ_LOG_ERROR("AssetBrowserItems: ModelAsset load failed [%s]", previewModelPath.c_str());
                MarkPreviewFailed(preview);
            }
        }
        /// @note マテリアルスロットを初回ロード (materials/slotName.mat -> per-slot MaterialPreview)
        if (preview.handle.IsValid() && !preview.materialsLoaded) {
            preview.materialsLoaded = true;
            const std::string matDir = util::FileSystem::NormalizePathSeparators(
                util::FileSystem::PathToUtf8(
                    ResolveModelGeneratedDir(previewModelPath, "materials")));
            if (const asset::ModelAsset* m0 = asset::AssetManager::Get(preview.handle)) {
                preview.slotMaterials.resize(m0->materialSlotNames.size());
                for (size_t si = 0; si < m0->materialSlotNames.size(); ++si) {
                    MaterialPreview& mp = preview.slotMaterials[si];
                    const std::string matPath = matDir + "/" + m0->materialSlotNames[si] + ".mat";
                    mp.failed = !asset::LoadMaterialAssetFromFile(matPath, mp.asset);
                    mp.loaded = true;
                }
            }
        }
        if (!preview.thumbnailRendered && preview.thumbnailRT.IsValid() && CanAttemptPreview(preview)) {
            const asset::ModelAsset* m = asset::AssetManager::Get(preview.handle);
            if (m && !m->lods.empty() && !m->lods[0].submeshes.empty()) {
                /// @note 全サブメッシュの AABB から共通カメラを計算 (Unity 同様すべてのメッシュが写る)
                constexpr float kInf = std::numeric_limits<float>::max();
                math::Vector3 bMin = { kInf,  kInf,  kInf  };
                math::Vector3 bMax = { -kInf, -kInf, -kInf };
                for (const auto& sub : m->lods[0].submeshes) {
                    if (!sub.mesh) continue;
                    const math::Vector3 c = sub.mesh->boundsCenter;
                    const float r = sub.mesh->boundsRadius;
                    bMin.x = std::min(bMin.x, c.x - r);  bMax.x = std::max(bMax.x, c.x + r);
                    bMin.y = std::min(bMin.y, c.y - r);  bMax.y = std::max(bMax.y, c.y + r);
                    bMin.z = std::min(bMin.z, c.z - r);  bMax.z = std::max(bMax.z, c.z + r);
                }
                const math::Vector3 combinedCenter = {
                    (bMin.x + bMax.x) * 0.5f, (bMin.y + bMax.y) * 0.5f, (bMin.z + bMax.z) * 0.5f };
                const float combinedRadius = std::max({ bMax.x - bMin.x,
                                                        bMax.y - bMin.y,
                                                        bMax.z - bMin.z }) * 0.5f;

                bool firstDraw = true;
                for (const auto& sub : m->lods[0].submeshes) {
                    if (!sub.mesh) continue;
                    auto* mesh = sub.mesh.get();
                    /// @note resources 未初期化時にロードされた場合 GPU バッファが未作成。CPU データから lazily 作成。
                    if (!mesh->vertexBuffer.IsValid()) {
                        if (!mesh->cpuVertices.empty())
                            mesh->vertexBuffer = ctx.resources->CreateVertexBuffer(
                                mesh->cpuVertices.data(),
                                mesh->cpuVertices.size() * sizeof(renderer::Vertex),
                                sizeof(renderer::Vertex));
                        else if (!mesh->cpuSkinnedVertices.empty())
                            mesh->vertexBuffer = ctx.resources->CreateVertexBuffer(
                                mesh->cpuSkinnedVertices.data(),
                                mesh->cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex),
                                sizeof(renderer::SkinnedVertex));
                    }
                    if (!mesh->indexBuffer.IsValid() && !mesh->cpuIndices.empty())
                        mesh->indexBuffer = ctx.resources->CreateIndexBuffer(
                            mesh->cpuIndices.data(), static_cast<uint32_t>(mesh->cpuIndices.size()));

                    MaterialPreview* matPrev = nullptr;
                    if (sub.materialSlotIndex < preview.slotMaterials.size() &&
                        !preview.slotMaterials[sub.materialSlotIndex].failed) {
                        matPrev = &preview.slotMaterials[sub.materialSlotIndex];
                        const matpreview::Flavor slotFlavor = matpreview::DetectFlavor(matPrev->asset);
                        /// @note メッシュ本体の形へ焼くので、板 / 矩形前提の Flavor は材質ごと落とす。
                        if (slotFlavor != matpreview::Flavor::Surface &&
                            slotFlavor != matpreview::Flavor::Skinned) {
                            matPrev = nullptr;
                        } else if (!RebuildMaterialThumbnailGpuData(*matPrev, ctx)) {
                            matPrev = nullptr;
                        }
                    }
                    const bool ok = RenderMeshThumbnail(
                        *ctx.renderer, *ctx.resources,
                        *mesh, preview.thumbnailRT,
                        renderer::ResourceHandle<renderer::TextureTag>{},
                        matPrev ? SelectMaterialColor(matPrev->asset) : ImVec4{ 0.74f, 0.78f, 0.84f, 1.0f },
                        matPrev ? &matPrev->gpu : nullptr,
                        matPrev ? matpreview::DetectFlavor(matPrev->asset) : matpreview::Flavor::Surface,
                        matPrev ? &matPrev->asset : nullptr,
                        firstDraw, combinedCenter, combinedRadius);
                    if (ok) { preview.thumbnailRendered = true; firstDraw = false; }
                }
            }
            if (preview.thumbnailRendered) MarkPreviewSucceeded(preview);
            else                           MarkPreviewFailed(preview);
        }
        if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "FBX")) return;
    }

    /// @note 仮想 .mesh サブアセット (::mesh:: 合成パス): .fzasset 内の特定サブメッシュを 3D プレビュー
    {
        const auto mark = e.path.find("::mesh::");
        if (e.ext == ".mesh" && e.isSubAsset && mark != std::string::npos &&
            ctx.renderer && ctx.resources && ctx.imguiRenderer) {
            const std::string parentPath = e.path.substr(0, mark);
            const auto submeshIdx = static_cast<size_t>(std::stoi(e.path.substr(mark + 8)));

            ModelAssetPreview& preview = m_modelAssetPreviews[e.path];
            const auto parentWriteTime = ReadLastWriteTime(parentPath);
            if (parentWriteTime != preview.lastWriteTime) {
                preview.lastWriteTime    = parentWriteTime;
                preview.handle           = {};
                preview.thumbnailRendered = false;
                MarkPreviewSucceeded(preview);
            }
            EnsureThumbnailRT(preview, ctx);
            if (!preview.handle.IsValid() && CanAttemptPreview(preview)) {
                asset::AssetManager::FlushFailed();
                preview.handle = asset::AssetManager::Load<asset::ModelAsset>(parentPath);
                if (preview.handle.IsValid()) MarkPreviewSucceeded(preview);
                else                          MarkPreviewFailed(preview);
            }
            if (!preview.thumbnailRendered && preview.thumbnailRT.IsValid() && CanAttemptPreview(preview)) {
                const asset::ModelAsset* m = asset::AssetManager::Get(preview.handle);
                if (m && !m->lods.empty() && submeshIdx < m->lods[0].submeshes.size() &&
                    m->lods[0].submeshes[submeshIdx].mesh) {
                    auto* mesh = m->lods[0].submeshes[submeshIdx].mesh.get();
                    if (!mesh->vertexBuffer.IsValid()) {
                        if (!mesh->cpuVertices.empty())
                            mesh->vertexBuffer = ctx.resources->CreateVertexBuffer(
                                mesh->cpuVertices.data(),
                                mesh->cpuVertices.size() * sizeof(renderer::Vertex),
                                sizeof(renderer::Vertex));
                        else if (!mesh->cpuSkinnedVertices.empty())
                            mesh->vertexBuffer = ctx.resources->CreateVertexBuffer(
                                mesh->cpuSkinnedVertices.data(),
                                mesh->cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex),
                                sizeof(renderer::SkinnedVertex));
                    }
                    if (!mesh->indexBuffer.IsValid() && !mesh->cpuIndices.empty())
                        mesh->indexBuffer = ctx.resources->CreateIndexBuffer(
                            mesh->cpuIndices.data(), static_cast<uint32_t>(mesh->cpuIndices.size()));
                    preview.thumbnailRendered = RenderMeshThumbnail(
                        *ctx.renderer, *ctx.resources,
                        *mesh, preview.thumbnailRT,
                        renderer::ResourceHandle<renderer::TextureTag>{},
                        { 0.74f, 0.78f, 0.84f, 1.0f });
                }
                if (preview.thumbnailRendered) MarkPreviewSucceeded(preview);
                else                           MarkPreviewFailed(preview);
            }
            if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "MESH")) return;
        }
    }

    if (IsMeshExt(e.ext) && e.path.find("::mesh::") == std::string::npos && ctx.renderer && ctx.resources && ctx.imguiRenderer) {
        MeshPreview& preview = m_meshPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        if (currentWriteTime != preview.lastWriteTime) {
            preview.lastWriteTime = currentWriteTime;
            preview.model = nullptr;
            preview.thumbnailRendered = false;
            MarkPreviewSucceeded(preview);
        }
        EnsureThumbnailRT(preview, ctx);
        const bool meshRetryAllowed = CanAttemptPreview(preview);
        if (!preview.model && meshRetryAllowed)
            preview.model = asset::AssetManager::LoadAndGet<asset::Model>(e.path);
        if (!preview.thumbnailRendered && meshRetryAllowed && preview.thumbnailRT.IsValid()) {
            if (preview.model && !preview.model->meshes.empty() && preview.model->meshes.front()) {
                preview.thumbnailRendered = RenderMeshThumbnail(
                    *ctx.renderer,
                    *ctx.resources,
                    *preview.model->meshes.front(),
                    preview.thumbnailRT,
                    renderer::ResourceHandle<renderer::TextureTag>{},
                    { 0.74f, 0.78f, 0.84f, 1.0f });
            }
            if (preview.thumbnailRendered) MarkPreviewSucceeded(preview);
            else                           MarkPreviewFailed(preview);
        }
        const char* badge = (e.ext == ".asset") ? "ASSET" : "MESH";
        if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, badge)) return;
    }

    /// @note .prefab: TOML を解析してメッシュを持つ場合は 3D サムネイル、なければキューブアイコン
    if (e.ext == ".prefab") {
        if (ctx.renderer && ctx.resources && ctx.imguiRenderer) {
            PrefabPreview& preview = m_prefabPreviews[e.path];
            const auto currentWriteTime = ReadLastWriteTime(e.path);
            if (currentWriteTime != preview.lastWriteTime) {
                preview = {};
                preview.lastWriteTime = currentWriteTime;
            }
            if (!preview.parsed) {
                preview.parsed = true;
                std::string text;
                if (util::FileSystem::ReadText(e.path, text)) {
                    toml::parse_result result = toml::parse(text);
                    if (result) {
                        if (auto* gos = result.table()["gameobjects"].as_array()) {
                            for (const auto& item : *gos) {
                                const auto* goTbl = item.as_table();
                                if (!goTbl) continue;
                                /// @note SkinnedMeshRenderer を優先 (フルモデルパス)
                                if (auto* smrTbl = (*goTbl)["SkinnedMeshRenderer"].as_table()) {
                                    const std::string mp = (*smrTbl)["modelPath"].value_or(std::string{});
                                    if (!mp.empty()) {
                                        preview.meshPath = mp;
                                        preview.hasMesh = true;
                                        break;
                                    }
                                }
                                if (auto* mrTbl = (*goTbl)["MeshRenderer"].as_table()) {
                                    const std::string mp = (*mrTbl)["mesh"].value_or(std::string{});
                                    if (!mp.empty()) {
                                        preview.meshPath = mp;
                                        preview.hasMesh = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (preview.hasMesh && CanAttemptPreview(preview)) {
                EnsureThumbnailRT(preview, ctx);
                if (!preview.model) {
                    std::string absPath = preview.meshPath;
                    if (absPath.starts_with("Assets/") && !ctx.projectRoot.empty())
                        absPath = ctx.projectRoot + "/" + absPath;
                    preview.model = asset::AssetManager::LoadAndGet<asset::Model>(absPath);
                }
                if (!preview.thumbnailRendered && preview.thumbnailRT.IsValid()) {
                    if (preview.model && !preview.model->meshes.empty() && preview.model->meshes.front()) {
                        preview.thumbnailRendered = RenderMeshThumbnail(
                            *ctx.renderer, *ctx.resources,
                            *preview.model->meshes.front(),
                            preview.thumbnailRT,
                            renderer::ResourceHandle<renderer::TextureTag>{},
                            { 0.35f, 0.82f, 0.95f, 1.0f });
                    }
                    if (preview.thumbnailRendered) MarkPreviewSucceeded(preview);
                    else                           MarkPreviewFailed(preview);
                }
                if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "PREFAB")) return;
            }
        }
        /// @note フォールバック: アイソメトリックキューブアイコン
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 bg    = IM_COL32( 28,  68,  84, 255);
            const ImU32 front = IM_COL32( 50, 140, 168, 255);
            const ImU32 top   = IM_COL32( 72, 172, 200, 255);
            const ImU32 right = IM_COL32( 36, 108, 132, 255);
            const ImU32 brd   = IM_COL32(110, 215, 235, 255);
            dl->AddRectFilled({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, bg, sz * 0.08f);
            dl->AddRect      ({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, brd, sz * 0.08f, 0, 1.0f);

            const float cx = origin.x + sz * 0.48f;
            const float cy = origin.y + sz * 0.54f;
            /// @note front face half-width
            const float hw = sz * 0.22f;
            /// @note front face half-height
            const float hh = sz * 0.20f;
            /// @note depth x-offset
            const float dx = sz * 0.14f;
            /// @note depth y-offset
            const float dy = sz * 0.09f;

            /// @note front face
            ImVec2 frontFace[4] = {
                { cx - hw,      cy - hh },
                { cx + hw,      cy - hh },
                { cx + hw,      cy + hh },
                { cx - hw,      cy + hh },
            };
            dl->AddConvexPolyFilled(frontFace, 4, front);

            /// @note top face
            ImVec2 topFace[4] = {
                { cx - hw,      cy - hh      },
                { cx + hw,      cy - hh      },
                { cx + hw + dx, cy - hh - dy },
                { cx - hw + dx, cy - hh - dy },
            };
            dl->AddConvexPolyFilled(topFace, 4, top);

            /// @note right face
            ImVec2 rightFace[4] = {
                { cx + hw,      cy - hh      },
                { cx + hw + dx, cy - hh - dy },
                { cx + hw + dx, cy + hh - dy },
                { cx + hw,      cy + hh      },
            };
            dl->AddConvexPolyFilled(rightFace, 4, right);

            dl->AddPolyline(frontFace, 4, brd, ImDrawFlags_Closed, 1.0f);
            dl->AddPolyline(topFace,   4, brd, ImDrawFlags_Closed, 1.0f);
            dl->AddPolyline(rightFace, 4, brd, ImDrawFlags_Closed, 1.0f);
            DrawThumbnailLabel(dl, origin, sz, "PREFAB");
            return;
        }
    }

    /// @note .vfx: 主役エミッターの .mat から素材テクスチャ 1 枚を出す。中身を焼かないのは、
    ///       粒子は時間と GPU パスの産物で 1 枚絵にならないため。«何の絵か» だけ出して、
    ///       確かめる導線は Inspector の Open in Prefab Mode に預ける。
    if (e.ext == ".vfx" && ctx.resources && ctx.imguiRenderer) {
        VfxPreview& preview = m_vfxPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        if (currentWriteTime != preview.lastWriteTime) {
            preview = {};
            preview.lastWriteTime = currentWriteTime;
        }
        if (!preview.parsed) {
            preview.parsed = true;
            std::string text;
            if (util::FileSystem::ReadText(e.path, text)) {
                toml::parse_result result = toml::parse(text);
                if (result) {
                    if (auto* gos = result.table()["gameobjects"].as_array()) {
                        /// @note 主役は一番手前に描かれるエミッター。同値なら粒の大きい方。
                        std::string heroMaterial;
                        int   heroPriority = (std::numeric_limits<int>::min)();
                        float heroSize     = -1.0f;
                        for (const auto& item : *gos) {
                            const auto* goTbl = item.as_table();
                            if (!goTbl) continue;
                            const auto* emitterTbl = (*goTbl)["ParticleEmitter"].as_table();
                            if (!emitterTbl) continue;
                            const std::string matPath =
                                (*emitterTbl)["materialPath"].value_or(std::string{});
                            if (matPath.empty()) continue;
                            const int   priority =
                                static_cast<int>((*emitterTbl)["renderPriority"].value_or(int64_t{ 0 }));
                            const float size =
                                static_cast<float>((*emitterTbl)["sizeStart"].value_or(0.0));
                            if (priority > heroPriority ||
                                (priority == heroPriority && size > heroSize)) {
                                heroPriority = priority;
                                heroSize     = size;
                                heroMaterial = matPath;
                            }
                        }
                        if (!heroMaterial.empty()) {
                            std::string absMatPath = heroMaterial;
                            if (absMatPath.starts_with("Assets/") && !ctx.projectRoot.empty())
                                absMatPath = ctx.projectRoot + "/" + absMatPath;
                            asset::MaterialAsset heroAsset;
                            if (asset::LoadMaterialAssetFromFile(absMatPath, heroAsset)) {
                                const std::string texPath = SelectMaterialPreviewTexture(heroAsset);
                                if (!texPath.empty()) {
                                    preview.texturePath = ToTextureLoadPath(texPath, ctx);
                                    preview.hasTexture  = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (preview.hasTexture) {
            /// @note 読み込みは通常のテクスチャサムネイルと同じ列に積む (3 件/フレーム)。
            TexturePreview& texture = m_texturePreviews[preview.texturePath];
            if (!texture.handle.IsValid() && !texture.queued && CanAttemptPreview(texture)) {
                texture.queued = true;
                m_texLoadQueue.push_back(preview.texturePath);
            }
            if (texture.handle.IsValid()) {
                if (void* rawID = ctx.imguiRenderer->GetImTextureID(texture.handle, *ctx.resources)) {
                    DrawSpriteThumbnail(rawID, texture.width, texture.height,
                                        nullptr, "VFX", origin, sz, hovered);
                    return;
                }
            }
        }
        /// @note 素材を引けなければ拡張子アイコンへ落とす (末尾の DrawFileIconAt)。
    }

    /// @note .animcontroller: ステートマシン風アイコン (3ノード + 矢印)
    if (e.ext == ".animcontroller") {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg   = IM_COL32( 50,  90,  65, 255);
        const ImU32 node = IM_COL32( 80, 200, 120, 255);
        const ImU32 edge = IM_COL32(200, 255, 200, 180);
        const ImU32 brd  = IM_COL32(160, 220, 160, 255);
        dl->AddRectFilled({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, bg, sz * 0.08f);
        dl->AddRect      ({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, brd, sz * 0.08f, 0, 1.0f);
        /// @note node positions: left-mid, top-right, bottom-right
        const float r = sz * 0.10f;
        const ImVec2 n0 = { origin.x + sz * 0.22f, origin.y + sz * 0.50f };
        const ImVec2 n1 = { origin.x + sz * 0.65f, origin.y + sz * 0.28f };
        const ImVec2 n2 = { origin.x + sz * 0.65f, origin.y + sz * 0.72f };
        dl->AddLine(n0, n1, edge, 1.0f);
        dl->AddLine(n0, n2, edge, 1.0f);
        dl->AddLine(n1, n2, edge, 1.0f);
        dl->AddCircleFilled(n0, r * 1.2f, node);
        dl->AddCircleFilled(n1, r,        node);
        dl->AddCircleFilled(n2, r,        node);
        DrawThumbnailLabel(dl, origin, sz, "CTRL");
        return;
    }

    /// @note .terrain: 最初に .mat を持つ層の diffuse でサムネイル、なければ丘アイコン
    if (e.ext == ".terrain") {
        if (ctx.renderer && ctx.resources && ctx.imguiRenderer) {
            TerrainPreview& preview = m_terrainPreviews[e.path];
            const auto currentWriteTime = ReadLastWriteTime(e.path);
            if (currentWriteTime != preview.lastWriteTime) {
                preview = {};
                preview.lastWriteTime = currentWriteTime;
            }
            if (!preview.parsed) {
                preview.parsed = true;
                std::string text;
                if (util::FileSystem::ReadText(e.path, text)) {
                    toml::parse_result result = toml::parse(text);
                    if (result) {
                        const toml::table* terrainTbl = result.table()["terrain"].as_table();
                        if (!terrainTbl) terrainTbl = &result.table();
                        std::string matPath;
                        /// @note 層数は可変で、先頭の層が空 (既定の白) のこともある。最初に .mat を持つ層で描く。
                        if (const auto* layerArr = (*terrainTbl)["layerMaterials"].as_array()) {
                            for (const toml::node& layerNode : *layerArr) {
                                matPath = layerNode.value_or(std::string{});
                                if (!matPath.empty()) break;
                            }
                        }
                        if (!matPath.empty()) {
                            std::string absMatPath = matPath;
                            if (absMatPath.starts_with("Assets/") && !ctx.projectRoot.empty())
                                absMatPath = ctx.projectRoot + "/" + absMatPath;
                            preview.hasMaterial = asset::LoadMaterialAssetFromFile(absMatPath, preview.mat.asset);
                            preview.mat.loaded = true;
                            preview.mat.lastWriteTime = ReadLastWriteTime(absMatPath);
                        }
                    }
                }
            }
            if (preview.hasMaterial && CanAttemptPreview(preview.mat)) {
                EnsureThumbnailRT(preview.mat, ctx);
                if (!preview.mat.thumbnailRendered) {
                    renderer::Mesh* terrainMesh = matpreview::ShapeMesh(
                        *ctx.resources, matpreview::Shape::Plane, matpreview::Flavor::Terrain);
                    if (terrainMesh && RebuildMaterialThumbnailGpuData(preview.mat, ctx)) {
                        preview.mat.thumbnailRendered = RenderMeshThumbnail(
                            *ctx.renderer, *ctx.resources,
                            *terrainMesh,
                            preview.mat.thumbnailRT,
                            preview.mat.previewTexture,
                            SelectMaterialColor(preview.mat.asset),
                            &preview.mat.gpu,
                            matpreview::Flavor::Terrain,
                            &preview.mat.asset);
                        if (preview.mat.thumbnailRendered) MarkPreviewSucceeded(preview.mat);
                        else                               MarkPreviewFailed(preview.mat);
                    }
                }
                if (DrawThumbnailIfReady(preview.mat, origin, sz, ctx, hovered, "TERRAIN")) return;
            }
        }
        /// @note フォールバック: 丘シルエットアイコン
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 sky  = IM_COL32( 60, 100,  60, 255);
            const ImU32 land = IM_COL32( 80, 160,  70, 255);
            const ImU32 brd  = IM_COL32(140, 210, 120, 255);
            dl->AddRectFilled({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, sky, sz * 0.08f);
            dl->AddRect      ({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, brd, sz * 0.08f, 0, 1.0f);
            const float base = origin.y + sz * 0.95f;
            const float h    = sz * 0.40f;
            ImVec2 terrain[8] = {
                { origin.x,           base          },
                { origin.x + sz*0.0f, base          },
                { origin.x + sz*0.2f, base - h*0.5f },
                { origin.x + sz*0.4f, base - h      },
                { origin.x + sz*0.6f, base - h*0.6f },
                { origin.x + sz*0.8f, base - h*0.8f },
                { origin.x + sz,      base - h*0.3f },
                { origin.x + sz,      base          },
            };
            dl->AddConvexPolyFilled(terrain, 8, land);
            DrawThumbnailLabel(dl, origin, sz, "TERRAIN");
            return;
        }
    }

    DrawFileIconAt(origin, sz, e, hovered);
}

void AssetBrowserPanel::DrainTexLoadQueue(EditorContext& ctx)
{
    if (!ctx.resources) return;
    constexpr int kMaxPerFrame = 3;
    for (int i = 0; i < kMaxPerFrame && !m_texLoadQueue.empty(); ++i) {
        const std::string path = std::move(m_texLoadQueue.front());
        m_texLoadQueue.pop_front();
        auto it = m_texturePreviews.find(path);
        if (it == m_texturePreviews.end()) continue;
        TexturePreview& preview = it->second;
        /// @note キューから出した時点で「積んである」印を落とす。ここで戻さないと、
        ///       再試行に回すべきエントリが二度と積み直されない。
        preview.queued = false;
        if (preview.handle.IsValid()) continue;
        if (util::StringUtils::ToLower(util::FileSystem::GetExtension(path)) == ".ico") {
            preview.handle = LoadIcoTexture(*ctx.resources, ToTextureLoadPath(path, ctx),
                                            preview.width, preview.height);
            preview.ownsTexture = preview.handle.IsValid();
            if (preview.handle.IsValid()) MarkPreviewSucceeded(preview);
            else                          MarkPreviewFailed(preview);
            continue;
        }
        preview.handle = ctx.resources->LoadTexture(ToTextureLoadPath(path, ctx));
        if (preview.handle.IsValid()) {
            MarkPreviewSucceeded(preview);
            if (auto* texture = ctx.resources->Get(preview.handle)) {
                preview.width  = texture->GetWidth();
                preview.height = texture->GetHeight();
            }
        } else {
            MarkPreviewFailed(preview);
        }
    }
}

void AssetBrowserPanel::ReleaseOwnedTexturePreview(const std::string& path)
{
    auto it = m_texturePreviews.find(path);
    if (it == m_texturePreviews.end()) return;
    if (m_resources && it->second.ownsTexture && it->second.handle.IsValid())
        m_resources->Release(it->second.handle);
    it->second.handle = {};
    it->second.ownsTexture = false;
}

void AssetBrowserPanel::ResetAssetPreviewCache(const std::string& path)
{
    ReleaseOwnedTexturePreview(path);
    m_texturePreviews.erase(path);
    m_spritePreviews.erase(path);
    auto releaseAndErase = [&](auto& map) {
        auto it = map.find(path);
        if (it == map.end()) return;
        if (m_resources) {
            if constexpr (requires { it->second.thumbnailRT; }) {
                if (it->second.thumbnailRT.IsValid())
                    m_resources->Release(it->second.thumbnailRT);
                if constexpr (requires { it->second.gpu; })
                    matpreview::ResetGpuData(it->second.gpu, m_resources);
            } else if constexpr (requires { it->second.mat.thumbnailRT; }) {
                if (it->second.mat.thumbnailRT.IsValid())
                    m_resources->Release(it->second.mat.thumbnailRT);
                matpreview::ResetGpuData(it->second.mat.gpu, m_resources);
            }
        }
        map.erase(it);
    };
    releaseAndErase(m_materialPreviews);
    releaseAndErase(m_meshPreviews);
    releaseAndErase(m_prefabPreviews);
    releaseAndErase(m_vfxPreviews);
    releaseAndErase(m_terrainPreviews);
    {
        auto it = m_modelAssetPreviews.find(path);
        if (it != m_modelAssetPreviews.end() && m_resources)
            for (auto& slot : it->second.slotMaterials)
                matpreview::ResetGpuData(slot.gpu, m_resources);
    }
    releaseAndErase(m_modelAssetPreviews);
    /// @note 合成パス (path::mesh::N) で登録されたサブメッシュプレビューもクリア
    {
        const std::string synthPrefix = path + "::mesh::";
        for (auto it = m_modelAssetPreviews.begin(); it != m_modelAssetPreviews.end(); ) {
            if (it->first.starts_with(synthPrefix)) {
                if (m_resources) {
                    if (it->second.thumbnailRT.IsValid())
                        m_resources->Release(it->second.thumbnailRT);
                    for (auto& slot : it->second.slotMaterials)
                        matpreview::ResetGpuData(slot.gpu, m_resources);
                }
                it = m_modelAssetPreviews.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_texDescPreviews.erase(path);
}

void AssetBrowserPanel::ClearAllAssetPreviews()
{
    auto releaseAll = [&](auto& map) {
        if (m_resources) {
            for (auto it = map.begin(); it != map.end(); ++it) {
                auto& preview = it->second;
                if constexpr (requires { preview.thumbnailRT; }) {
                    if (preview.thumbnailRT.IsValid())
                        m_resources->Release(preview.thumbnailRT);
                    if constexpr (requires { preview.gpu; })
                        matpreview::ResetGpuData(preview.gpu, m_resources);
                    if constexpr (requires { preview.slotMaterials; }) {
                        for (auto& slot : preview.slotMaterials)
                            matpreview::ResetGpuData(slot.gpu, m_resources);
                    }
                } else if constexpr (requires { preview.mat.thumbnailRT; }) {
                    if (preview.mat.thumbnailRT.IsValid())
                        m_resources->Release(preview.mat.thumbnailRT);
                    matpreview::ResetGpuData(preview.mat.gpu, m_resources);
                }
            }
        }
        map.clear();
    };
    releaseAll(m_materialPreviews);
    releaseAll(m_meshPreviews);
    releaseAll(m_prefabPreviews);
    releaseAll(m_terrainPreviews);
    releaseAll(m_modelAssetPreviews);
    /// @note 自前で作った実体 (.ico) だけは解放する。他は ResourceManager 側のキャッシュを共有する。
    if (m_resources) {
        for (auto& [path, preview] : m_texturePreviews)
            if (preview.ownsTexture && preview.handle.IsValid())
                m_resources->Release(preview.handle);
    }
    m_texturePreviews.clear();
    m_texDescPreviews.clear();
    m_spritePreviews.clear();
    m_vfxPreviews.clear();
    m_texLoadQueue.clear();
}

void AssetBrowserPanel::ResyncAfterWatcherOverflow()
{
    m_treeCache.clear();
    m_assetSubItemsCache.clear();
    ClearAllAssetPreviews();

    /// @note 取りこぼした Added の分だけ .meta / guid が発行されていない。
    ///       GuidFromPath は .meta を持つべき拡張子だけを対象に、無ければ発行して索引へ入れる
    ///       (FBX は Import まで原本の .meta を作らないため除く)。
    for (const std::filesystem::path& p :
         util::FileSystem::ListFilesRecursive(util::FileSystem::PathFromUtf8(m_rootPath))) {
        const std::string absPath = util::FileSystem::NormalizePathSeparators(
            util::FileSystem::PathToUtf8(p));
        const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath));
        if (ext == ".meta" || ext == ".fbx") continue;
        (void)asset::AssetDatabase::GuidFromPath(absPath);
    }

    AssetSearch::Rebuild();
    if (!util::FileSystem::IsDirectory(m_currentPath))
        m_currentPath = m_rootPath;
    RefreshDirectory();
    ScanAndQueueUnimported(m_rootPath);
}

/// @name DrawEntry サブメソッド

void AssetBrowserPanel::DrawEntryBadges(ImDrawList* dl, ImVec2 origin, float sz, const Entry& e)
{
    /// @note 選択・ホバー表現は DrawEntry 側 (SelectionVisuals) が担当する。ここは状態バッジのみ。
    ///       ! バッジ: 未変換モデルファイルに赤丸で警告表示。FBX は正規モデルアセットとして
    ///       扱うため、未変換警告は出さない。
    if (!e.isDir && IsImportableRaw(e.ext) && e.ext != ".fbx") {
        const float r  = sz * 0.15f;
        const float cx = origin.x + sz - r;
        const float cy = origin.y + r;
        dl->AddCircleFilled({ cx, cy }, r, IM_COL32(220, 50, 50, 230));
        const ImVec2 bsz = ImGui::CalcTextSize("!");
        dl->AddText({ cx - bsz.x * 0.5f, cy - bsz.y * 0.5f }, IM_COL32(255, 255, 255, 255), "!");
    }
    /// @note 橙ドット: 未保存変更があるアセット
    ///       (「再インポートが必要」の印は廃止。ウォッチャーが自動で焼き直し、進行は
    ///       EditorTaskOverlay に出る)
    if (!e.isDir && AssetDirtyRegistry::IsDirty(e.path)) {
        const float r  = sz * 0.10f;
        const float cx = origin.x + r + 2.0f;
        const float cy = origin.y + r + 2.0f;
        dl->AddCircleFilled({ cx, cy }, r, IM_COL32(255, 160, 30, 230));
    }
    /// @note ▶/▼ 展開トグル: FBX と Sprite Texture はサブアセットを持つ。
    ///       素の三角形はサムネイルの絵柄に溶けるので、暗いチップに乗せて押せる場所だと分からせる。
    if (!e.isDir && !e.isSubAsset && e.hasSubAssets) {
        const bool  expanded = m_expandedAssets.count(e.path) > 0;
        /// @note 三角サイズ (クリック判定と共通)
        const float ts   = sz * 0.18f;
        const float bx   = origin.x + 2.0f;
        const float by   = origin.y + sz - ts - 2.0f;
        const float pad  = ts * 0.35f;
        dl->AddRectFilled({ bx - pad, by - pad }, { bx + ts + pad, by + ts + pad },
                          IM_COL32(18, 20, 24, 190), 4.0f);
        const ImU32 col = expanded
            ? EditorTheme::ColorU32(ThemeColor::Accent, 1.0f)
            : IM_COL32(226, 232, 240, 235);
        if (expanded) {
            /// @note ▼ (pointing down)
            dl->AddTriangleFilled(
                { bx,        by },
                { bx + ts,   by },
                { bx + ts * 0.5f, by + ts },
                col);
        } else {
            /// @note ▶ (pointing right)
            dl->AddTriangleFilled(
                { bx,        by },
                { bx,        by + ts },
                { bx + ts,   by + ts * 0.5f },
                col);
        }
    }
    /// @note サブアセットの帯 (親と子を繋ぐ面) は DrawEntry 側の DrawSubAssetBand が描く。
}

void AssetBrowserPanel::HandleEntryClick(const Entry& e, EditorContext& ctx, bool hov)
{
    /// @note マウス押下フレーム: ドラッグ・ダブルクリックフラグをリセット (選択はまだしない)
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_entryDragStarted    = false;
        m_doubleClickConsumed = false;
    }

    /// @note 選択確定はマウスリリース時 (Unity スタイル: D&D 開始後はスキップ)
    if (!hov || !ImGui::IsMouseReleased(ImGuiMouseButton_Left) || e.isDir) return;
    if (m_entryDragStarted) return;
    if (m_doubleClickConsumed) { m_doubleClickConsumed = false; return; }

    const bool ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);

    if (ctrl) {
        if (m_selectedPaths.count(e.path)) m_selectedPaths.erase(e.path);
        else                                m_selectedPaths.insert(e.path);
        SelectAsset(ctx, e.path);
        m_lastClickedPath     = e.path;
        m_pendingRenamePath.clear();
    } else if (shift && !m_lastClickedPath.empty()) {
        m_selectedPaths.clear();
        bool inside = false;
        for (const Entry& entry : m_entries) {
            if (entry.isDir) continue;
            if (entry.path == m_lastClickedPath || entry.path == e.path) {
                inside = !inside;
                m_selectedPaths.insert(entry.path);
            } else if (inside) {
                m_selectedPaths.insert(entry.path);
            }
        }
        SelectAsset(ctx, e.path);
        m_pendingRenamePath.clear();
    } else {
        /// @note 選択済み & 単体選択状態での再クリック → 遅延リネーム (Unity スタイル)
        ///       合成パス (::mesh:: 仮想サブアセット) はリネーム不可
        const bool canRename = !e.isMount && !e.isPackageAsset && !e.isSubAsset;
        if (canRename && ctx.selectedAssetPath == e.path && m_selectedPaths.empty()) {
            m_pendingRenamePath  = e.path;
            m_pendingRenameTimer = static_cast<float>(ImGui::GetTime());
            /// @note 待っている間にどれだけ動いたかを測る原点。ここから離れたら «開く / 掴む» とみなす。
            m_pendingRenameMouse = ImGui::GetIO().MousePos;
        } else {
            m_selectedPaths.clear();
            SelectAsset(ctx, e.path);
            m_lastClickedPath     = e.path;
            m_pendingRenamePath.clear();
            /// @note FBX コンテンツ更新をクリック時に実施 (ホバーから移行)
            if (IsMeshExt(e.ext)) {
                m_selectedFbxPath = e.path;
                m_selectedModel   = nullptr;
                if (renderer::ResourceManager::Active())
                    m_selectedModel = asset::AssetManager::LoadAndGet<asset::Model>(e.path);
            } else {
                m_selectedFbxPath.clear();
                m_selectedModel = nullptr;
            }
        }
    }
}

void AssetBrowserPanel::HandleEntryDoubleClick(const Entry& e, EditorContext& ctx, bool hov)
{
    if (!hov || !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) return;
    /// @note ダブルクリックは遅延リネームをキャンセル
    m_pendingRenamePath.clear();
    /// @note 2回目リリースで HandleEntryClick をスキップ
    m_doubleClickConsumed = true;

    /// @note ダブルクリック後に entries が更新される可能性があるため値をコピーする。
    const bool        isDir = e.isDir;
    const std::string path  = e.path;
    const std::string ext   = e.ext;

    if (isDir) { m_pendingNavigate = path; return; }

    if (e.isSpriteSubAsset && ctx.openSpriteEditor) {
        ctx.openSpriteEditor(e.sourceAssetPath + ".meta");
    } else if (ext == ".scene" && ctx.activeScene) {
        if (ctx.requestOpenScene) {
            ctx.requestOpenScene(path);
        } else if (SceneIO::Load(*ctx.activeScene, path)) {
            ClearEntitySelection(ctx);
            if (ctx.undoStack) ctx.undoStack->Clear();
            if (ctx.markSceneDirty) ctx.markSceneDirty();
            FBZZ_LOG_INFO("Opened scene: %s", path.c_str());
        } else {
            FBZZ_LOG_ERROR("Failed to open scene: %s", path.c_str());
        }
    } else if (ext == ".prefab" && ctx.activeScene &&
               (ImGui::GetIO().KeyAlt || ctx.InPrefabEditMode())) {
        /// @note Alt+ダブルクリック: シーンへ置くのではなく、プレファブ本体を編集面で開く。既定は
        ///       «配置» のままで、編集は破壊的になりうるため明示的な修飾キーと右クリックメニューから
        ///       だけ入れる。ただし編集面に居る間は Alt 無しでも開く — «別のプレハブへ移る» が主な
        ///       用件でそこへの導線が他に無いため (置く方は引き続き Hierarchy / Scene View へのドロップ)。
        ctx.requestOpenPrefabEdit = NormalizeAssetPath(path);
    } else if (ext == ".prefab" && ctx.activeScene) {
        const bool canRecordUndo =
            ctx.undoStack != nullptr && ctx.undoStack->IsRecordingEnabled();
        const std::string before = canRecordUndo
            ? SceneIO::Serialize(*ctx.activeScene)
            : std::string{};
        std::vector<scene::EntityID> roots;
        if (PrefabSerializer::Instantiate(*ctx.activeScene, path, roots)) {
            SelectEntities(ctx, roots);
            const std::string after = canRecordUndo
                ? SceneIO::Serialize(*ctx.activeScene)
                : std::string{};
            if (canRecordUndo && before != after) {
                scene::Scene* scene = ctx.activeScene;
                EditorContext* context = &ctx;
                const auto markDirty = ctx.markSceneDirty;
                auto restore = [scene, context, markDirty](const std::string& snapshot) {
                    if (SceneIO::Deserialize(*scene, snapshot)) {
                        ClearEntitySelection(*context);
                        if (markDirty) markDirty();
                    }
                };
                ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                    "Instantiate Prefab",
                    [restore, after]() { restore(after); },
                    [restore, before]() { restore(before); }));
            }
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
    } else if (ext == ".animcontroller" || ext == ".vfx" || ext == ".behaviortree"
               || ext == ".synth" || ext == ".sequence") {
        /// @note ドキュメント面へ渡す振り分けは asset.open operator が持つ。分岐を写すと、
        ///       対応拡張子を足したときに一部の経路だけ取りこぼす。
        OpArgs args;
        args.Set("path", path);
        InvokeOperator(ctx, "asset.open", args);
    }
}

/// @brief 選択中パスのスナップショットをクリップボードに積む。OS クリップボードではなく
/// @brief panel ローカルなのは、プロセス跨ぎの貼り付けが対象外で、Copy Path と役割が違うため。
void AssetBrowserPanel::CopySelectionToClipboard()
{
    m_clipboardPaths.clear();
    if (!m_selectedPaths.empty()) {
        m_clipboardPaths.assign(m_selectedPaths.begin(), m_selectedPaths.end());
    } else if (!m_lastClickedPath.empty() && util::FileSystem::Exists(m_lastClickedPath)) {
        m_clipboardPaths.push_back(m_lastClickedPath);
    }
}

/// @brief 現在開いているフォルダへクリップボードの内容を複製する。
/// @brief 生成した実体は Undo 対象外 (取り消したいときは Delete でごみ箱へ送る)。
/// @brief Library/Baked からのコピペは .meta を複製しない仕様のおかげで Extract と同じ結果になる。
/// @brief これを取り出しの正式な動線として認め、そう説明する。
void AssetBrowserPanel::PasteClipboardAssets(EditorContext& ctx)
{
    if (m_clipboardPaths.empty()) return;
    const std::string destDir = m_currentPath;

    std::vector<std::string> pastedPaths;
    int extractedCount = 0;
    for (const auto& srcPath : m_clipboardPaths) {
        /// @note 元がリネーム/削除済みなら黙ってスキップ
        if (!util::FileSystem::Exists(srcPath)) continue;
        if (m_packageAssetPaths.count(srcPath) > 0) continue;
        const bool isDir = util::FileSystem::IsDirectory(srcPath);
        const std::string dstPath = UniqueDestPath(srcPath, destDir, isDir);
        if (dstPath.empty()) continue;
        if (!CopyAssetPath(srcPath, dstPath, isDir)) {
            FBZZ_LOG_ERROR("Paste failed: %s -> %s", srcPath.c_str(), dstPath.c_str());
            continue;
        }
        if (IsBakedLibraryPath(srcPath)) ++extractedCount;
        pastedPaths.push_back(dstPath);
    }

    RefreshDirectory();

    /// @note 貼り付けた項目をそのまま選択状態にする (Unity と同じく直後にリネーム/移動しやすくする)。
    m_selectedPaths.clear();
    if (pastedPaths.size() == 1) {
        SelectAsset(ctx, pastedPaths.front());
        m_lastClickedPath     = pastedPaths.front();
    } else if (pastedPaths.size() > 1) {
        m_selectedPaths.insert(pastedPaths.begin(), pastedPaths.end());
        SelectAsset(ctx, pastedPaths.front());
    }

    /// @note 取り出しが起きたことは必ず伝える。黙って独立アセットが増えると、
    ///       「なぜ再インポートしても更新されないのか」が後から分からなくなる。
    if (extractedCount > 0) {
        Toast::Success(std::to_string(extractedCount) +
                       " generated asset(s) extracted to Assets");
    }
}

void AssetBrowserPanel::HandleClipboardShortcuts(EditorContext& ctx)
{
    if (!ctx.PanelScopeFocused(HotkeyScope::AssetBrowser)) return;
    /// @note リネーム中や検索ボックス入力中の Ctrl+C/V はテキスト編集として扱う (横取りしない)。
    if (ImGui::GetIO().WantTextInput) return;

    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
        CopySelectionToClipboard();
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
        PasteClipboardAssets(ctx);
}

bool AssetBrowserPanel::IsBakedLibraryPath(const std::string& absPath)
{
    /// @note 取り出し対象かどうかは拡張子ではなく「どこに居るか」で決まる。
    ///       Library/Baked = 再インポートで作り直される生成物、Assets = 人の著作物。
    const std::string normalized = util::StringUtils::ToLower(
        util::FileSystem::NormalizePathSeparators(absPath));
    return normalized.find("/library/baked/") != std::string::npos;
}

bool AssetBrowserPanel::IsExtractableSubAsset(const Entry& e)
{
    if (e.isDir || !e.isSubAsset) return false;
    /// @note 仮想サブアセット (Sprite の "path::id" / "::mesh::N") は実ファイルではない。
    if (e.isSpriteSubAsset) return false;
    if (e.path.find("::mesh::") != std::string::npos) return false;
    if (!util::FileSystem::Exists(e.path)) return false;

    /// @note Assets に居る .mat / textures は既に独立した実体なので、複製したいなら
    ///       通常の Duplicate を使えばよく、Extract という別概念を増やす必要が無い。
    return IsBakedLibraryPath(e.path);
}

std::string AssetBrowserPanel::ExtractSubAsset(const Entry& e, EditorContext& ctx) const
{
    if (!IsExtractableSubAsset(e)) return {};

    /// @note 取り出し先は原本 FBX の隣。取り出したクリップは、どのモデルから来たのかが
    ///       分からなくなると使い道が消えるため、原本と同じ場所に置いて対応を保つ。
    std::string destDir = m_currentPath;
    if (!e.sourceAssetPath.empty())
        destDir = util::FileSystem::GetDirectory(e.sourceAssetPath);
    if (destDir.empty()) destDir = ctx.projectRoot + "/Assets";
    /// @note GetDirectory は末尾に '/' を付けて返す。連結で "//" にならないよう落とす。
    while (!destDir.empty() && (destDir.back() == '/' || destDir.back() == '\\'))
        destDir.pop_back();
    if (!util::FileSystem::EnsureDirectory(destDir)) return {};

    const std::string fileName = util::FileSystem::GetFilename(e.path);
    std::string destPath = destDir + "/" + fileName;
    /// @note 既存を黙って上書きしない。2 回目の Extract は別名で残す。
    if (util::FileSystem::Exists(destPath)) {
        const std::size_t dot = fileName.rfind('.');
        const std::string stem = dot == std::string::npos ? fileName : fileName.substr(0, dot);
        const std::string ext  = dot == std::string::npos ? std::string{} : fileName.substr(dot);
        for (int suffix = 1; suffix < 10000; ++suffix) {
            destPath = destDir + "/" + stem + " " + std::to_string(suffix) + ext;
            if (!util::FileSystem::Exists(destPath)) break;
        }
    }

    if (!CopyAssetPath(e.path, destPath, false)) return {};

    /// @note .meta は複製しない。GUID をコピーすると 2 つの実体が同じ GUID を名乗り、
    ///       参照解決が不定になる。作らずに置けばスキャンが新しい GUID を採番する。
    return destPath;
}

void AssetBrowserPanel::DrawEntryContextMenu(const Entry& e, EditorContext& ctx)
{
    if (!ImGui::BeginPopupContextItem("##entry_ctx")) return;

    /// @note 複数選択時の一括操作
    const bool multiSel = m_selectedPaths.size() > 1 && m_selectedPaths.count(e.path);
    if (multiSel) {
        const int n = static_cast<int>(m_selectedPaths.size());
        char label[64];
        bool includesPackageAsset = false;
        for (const auto& path : m_selectedPaths) {
            if (m_packageAssetPaths.count(path) > 0) {
                includesPackageAsset = true;
                break;
            }
        }

        std::snprintf(label, sizeof(label), "Copy %d items", n);
        if (ImGui::MenuItem(label, "Ctrl+C"))
            CopySelectionToClipboard();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_clipboardPaths.empty()))
            PasteClipboardAssets(ctx);
        ImGui::Separator();

        std::snprintf(label, sizeof(label), "Duplicate %d items", n);
        ImGui::BeginDisabled(includesPackageAsset);
        if (ImGui::MenuItem(label)) {
            std::vector<std::string> paths(m_selectedPaths.begin(), m_selectedPaths.end());
            for (const auto& srcPath : paths) {
                const bool isDir = util::FileSystem::IsDirectory(srcPath);
                const std::string dstPath = UniqueDuplicatePath(srcPath, isDir);
                if (dstPath.empty()) continue;
                if (!CopyAssetPath(srcPath, dstPath, isDir))
                    FBZZ_LOG_ERROR("Duplicate failed: %s", srcPath.c_str());
            }
            RefreshDirectory();
        }
        ImGui::EndDisabled();

        std::snprintf(label, sizeof(label), "Delete %d items", n);
        ImGui::BeginDisabled(includesPackageAsset);
        if (ImGui::MenuItem(label)) {
            std::vector<std::string> paths(m_selectedPaths.begin(), m_selectedPaths.end());
            EditorContext* context = &ctx;
            ModalDialog::OpenConfirm("Delete",
                "Delete " + std::to_string(n) + " selected items?\n"
                "(moved to .fbzz/Trash — not undoable with Ctrl+Z)",
                [this, paths, context]() {
                    TrashAssets(paths, *context);
                    m_selectedPaths.clear();
                    RefreshDirectory();
                });
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::BeginMenu("Create")) { DrawCreateMenu(ctx); ImGui::EndMenu(); }
        ImGui::EndPopup();
        return;
    }

    /// @note Sprite サブアセットは実ファイルではないため、複製・削除・リネームを出さず、
    ///       元画像を編集する操作と安定参照のコピーだけを提供する。
    if (e.isSpriteSubAsset) {
        if (ctx.openSpriteEditor && ImGui::MenuItem("Open in Sprite Editor"))
            ctx.openSpriteEditor(e.sourceAssetPath + ".meta");
        if (ImGui::MenuItem("Copy Sprite Reference")) {
            const std::string projectReference = ToProjectAssetPath(e.path, ctx);
            ImGui::SetClipboardText(projectReference.c_str());
        }
        if (ImGui::MenuItem("Reveal Source in Explorer")) {
            const std::wstring sourcePath =
                util::FileSystem::PathFromUtf8(e.sourceAssetPath).wstring();
            const std::wstring args = L"/select," + sourcePath;
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ImGui::EndPopup();
        return;
    }

    /// @note Library/Baked に隔離された生成物 (.anim 等) を Assets へ取り出す
    ///       (Unity の "Extract From Prefab" 相当)。
    ///       隔離した .anim は再インポートのたびに上書きされるので、手で調整したいときは
    ///       上書きされない実体が要る。新しい GUID を振れば原本 FBX から切り離される。
    ///       元の参照は書き換えない。全部を新しい方へ向けると「複製したつもりが元も変わった」
    ///       ことになるので、差し替えるかどうかは人が決める。
    if (!e.isDir && e.isSubAsset && !e.isSpriteSubAsset && IsExtractableSubAsset(e)) {
        if (ImGui::MenuItem("Extract to Assets")) {
            const std::string extracted = ExtractSubAsset(e, ctx);
            if (extracted.empty()) {
                Toast::Error("Extract failed: " + e.name);
            } else {
                Toast::Success("Extracted " + util::FileSystem::GetFilename(extracted));
                /// @note 取り出した実体を選択状態にする。Ctrl+V は貼った項目を選択するため、
                ///       同じ «出して続けて編集する» 結果になる操作で選択の扱いが違うと迷う。
                m_pendingNavigate = util::FileSystem::GetDirectory(extracted);
                m_selectedPaths.clear();
                m_lastClickedPath     = extracted;
                SelectAsset(ctx, extracted);
                RefreshDirectory();
            }
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Copy this generated asset into Assets/ as an independent file.\n"
                "It stops being overwritten by reimport. Existing references keep\n"
                "pointing at the generated one until you reassign them.\n"
                "\n"
                "Ctrl+C then Ctrl+V does the same thing, but pastes into the\n"
                "folder you are currently viewing.");
        }
        ImGui::Separator();
    }

    /// @note プレファブ本体の編集面へ入る動線。既定のダブルクリックは «シーンへ配置» なので、
    ///       アセットそのものを直したいときの入口が無かった。シーンに 1 個も置いていない
    ///       プレファブも編集できる。
    if (!e.isDir && e.ext == ".prefab") {
        if (ImGui::MenuItem("Open Prefab (edit asset)")) {
            ctx.requestOpenPrefabEdit = NormalizeAssetPath(e.path);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Edit the .prefab itself — every instance follows on save\n"
                              "(Alt + double-click does the same)");
        ImGui::Separator();
    }

    if (!e.isDir && IsImportableRaw(e.ext)) {
        /// @note 原本と設定の変更はウォッチャーが自動で焼き直すので、ここは
        ///       「変更が無いのに作り直したい」ときの手動経路。
        ///       ラベルは m_outdatedPaths ではなく「既に入っているか」で出し分ける
        ///       (古い印はキュー投入から完了までの一瞬しか立たない)。
        const bool imported = IsAlreadyImported(e.path);
        if (ImGui::MenuItem(imported ? "\xe2\x86\xbb Re-import" : "Import")) {
            /// @note 自動経路と同じ「処理中」の印で二重投入を防ぐ (完了時に取り除かれる)。
            if (m_outdatedPaths.insert(e.path).second) {
                /// @note 保存済み設定を読み直してから積む。既定の options で押し流すと、
                ///       選択メッシュ・クリップ範囲・Loop Time が黙って初期値へ戻る。
                FbxImportOptions options{};
                (void)FbxMetaSerializer::LoadOptions(e.path, options);
                m_pendingImports.push_back({ e.path, std::move(options) });
                m_importAllRequested = true;
            }
        }
        if (ImGui::MenuItem("Import with Settings...")) {
            m_importSettings.path      = e.path;
            m_importSettings.options   = {};
            m_importSettings.open      = true;
            m_importSettings.visible   = true;
            m_importSettings.needsInit = true;
        }
        ImGui::Separator();
    }
    if (!e.isDir && IsTextureRaw(e.ext)) {
        const std::string metaPath = e.path + ".meta";
        const bool hasPendingImportSettings = AssetDirtyRegistry::IsDirty(metaPath);
        if (ctx.openSpriteEditor) {
            if (hasPendingImportSettings) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Open in Sprite Editor")) {
                asset::TextureAsset textureAsset;
                asset::TexDescSerializer serializer;
                (void)serializer.Load(metaPath, textureAsset);
                if (textureAsset.settings.type != asset::TextureType::Sprite) {
                    textureAsset.settings =
                        asset::DefaultSettingsForType(asset::TextureType::Sprite);
                }
                textureAsset.sourcePath = e.path;
                if (textureAsset.settings.sprites.empty()) {
                    asset::SpriteRect sprite;
                    sprite.id = util::GenerateUUID();
                    sprite.name = util::FileSystem::PathToUtf8(
                        util::FileSystem::PathFromUtf8(e.path).stem());
                    textureAsset.settings.sprites.push_back(std::move(sprite));
                }
                if (serializer.Save(textureAsset, metaPath)) {
                    ctx.requestAssetBrowserRefresh = true;
                    ctx.openSpriteEditor(metaPath);
                }
            }
            if (hasPendingImportSettings) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)
                && hasPendingImportSettings) {
                ImGui::SetTooltip("Apply or Revert the Inspector Import Settings first");
            }
        }
        if (ImGui::MenuItem("Import Settings...")) {
            m_textureImportSettings.path        = e.path;
            m_textureImportSettings.open        = true;
            m_textureImportSettings.visible     = true;
            m_textureImportSettings.needsInit   = true;
            m_textureImportSettings.fromWatcher = false;
        }
        ImGui::Separator();
    }
    if (!e.isMount && !e.isPackageAsset) {
        /// @note 右クリックした e が事前に左クリック選択されているとは限らないため、
        ///       m_lastClickedPath 頼みの CopySelectionToClipboard() ではなくこの項目自体を積む。
        const bool copiesGenerated = IsBakedLibraryPath(e.path);
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
            m_clipboardPaths = { e.path };
        /// @note 生成物を掴んだときは、貼り付けが «取り出し» になることを先に言う。結果として
        ///       独立アセットが増えるのに、操作名が Copy のままだと «再インポートしても
        ///       更新されない実体» を作った自覚が持てない。
        if (copiesGenerated) {
            ImGui::SameLine();
            ImGui::TextDisabled("(extracts on paste)");
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_clipboardPaths.empty()))
            PasteClipboardAssets(ctx);
        ImGui::Separator();
    }
    if (!e.isMount && !e.isPackageAsset && ImGui::MenuItem("Duplicate")) {
        const std::string dstPath = UniqueDuplicatePath(e.path, e.isDir);
        if (!dstPath.empty()) {
            if (CopyAssetPath(e.path, dstPath, e.isDir)) {
                RefreshDirectory();
                BeginRenameForPath(dstPath, &ctx);
            } else {
                FBZZ_LOG_ERROR("Duplicate failed: %s", e.path.c_str());
            }
        }
    }
    ImGui::Separator();
    if (!e.isDir && ImGui::MenuItem("Find References...")) {
        m_findRefs.targetPath = e.path;
        m_findRefs.results.clear();
        m_findRefs.open = true;

        /// @note 探すのは (1) GUID 参照、(2) パス参照 の 2 通り。
        ///       ディスク上の参照は保存時に "guid:<32hex>" へ変換される (GuidRefCodec) が、
        ///       baked アセットや guid を持たない参照はパスのまま残るため両方を見る。
        ///       拡張子を除いた stem 単独の一致は採らない ("Fire" のような短い名前が誤爆する)。
        const std::string guid = asset::AssetDatabase::TryGetGuidFromPath(e.path);
        const std::string guidRef = guid.empty()
            ? std::string{} : std::string(asset::AssetDatabase::kGuidPrefix) + guid;
        /// @note パス参照は "Assets/..." 起点で書かれる。
        const std::string relativePath = NormalizeAssetPath(e.path);

        for (const auto& p : util::FileSystem::ListFilesRecursive(
                util::FileSystem::PathFromUtf8(m_rootPath))) {
            const std::string scanPath = util::FileSystem::PathToUtf8(p);
            const std::string scanExt  = util::StringUtils::ToLower(
                util::FileSystem::GetExtension(scanPath));
            if (scanExt != ".scene" && scanExt != ".mat" && scanExt != ".prefab"
                && scanExt != ".animcontroller" && scanExt != ".vfx") continue;
            /// @note 自分自身は参照元に数えない。
            if (util::FileSystem::NormalizePathSeparators(scanPath)
                == util::FileSystem::NormalizePathSeparators(e.path)) continue;
            std::string content;
            util::FileSystem::ReadText(scanPath, content);
            const bool byGuid = !guidRef.empty() && content.find(guidRef) != std::string::npos;
            const bool byPath = !relativePath.empty() && content.find(relativePath) != std::string::npos;
            if (byGuid || byPath) {
                m_findRefs.results.push_back(
                    util::FileSystem::NormalizePathSeparators(scanPath));
            }
        }
    }
    /// @note 横断検索の結果からその場所へ移動する。検索でアセットを見つけた後、周辺のファイルも
    ///       見たいことが多いため、Explorer を開かずにブラウザ内で辿れるようにする。
    if (IsGlobalSearchActive() && !e.isDir && ImGui::MenuItem("Go to Containing Folder")) {
        /// @note GetDirectory は末尾に '/' を付けて返すため落とす (パス比較が壊れる)。
        std::string folder = util::FileSystem::GetDirectory(e.path);
        while (folder.size() > 1 && (folder.back() == '/' || folder.back() == '\\'))
            folder.pop_back();
        m_pendingNavigate = std::move(folder);
        /// @note 移動先では検索を解除しないと、そのフォルダの中身が見えない。
        m_searchBuf[0] = '\0';
        m_searchResultsQuery.clear();
        m_searchResultsTypeFilter = -1;
    }
    if (e.isDir && !e.isMount) {
        DrawFolderColorMenu(e.path, ctx);
        ImGui::Separator();
    }
    if (ImGui::MenuItem("Reveal in Explorer")) {
        const std::wstring wpath = util::FileSystem::PathFromUtf8(e.path).wstring();
        const std::wstring args  = L"/select," + wpath;
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::MenuItem("Copy Path")) {
        ImGui::SetClipboardText(e.path.c_str());
    }
    if (ImGui::MenuItem("Copy Project Path")) {
        const std::string projectPath = ToProjectAssetPath(e.path, ctx);
        ImGui::SetClipboardText(projectPath.c_str());
    }
    ImGui::Separator();
    ImGui::BeginDisabled(e.isMount || e.isPackageAsset);
    if (ImGui::MenuItem("Rename")) {
        BeginRenameForPath(e.path, &ctx);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete")) {
        const std::string path = e.path;
        EditorContext* context = &ctx;
        ModalDialog::OpenConfirm("Delete",
            "Delete \"" + util::FileSystem::GetFilename(path) + "\"?\n"
            "(moved to .fbzz/Trash — not undoable with Ctrl+Z)",
            [this, path, context]() {
                TrashAssets({ path }, *context);
                if (m_selectedFbxPath == path) { m_selectedFbxPath.clear(); m_selectedModel = nullptr; }
                m_selectedPaths.erase(path);
                ResetAssetPreviewCache(path);
                RefreshDirectory();
            });
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::BeginMenu("Create")) {
        DrawCreateMenu(ctx);
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::DrawEntryRenameLabel(const Entry& e, EditorContext& ctx)
{
    if (m_renamingPath == e.path) {
        /// @note 入力欄は拡張子ぶんの幅を空けて置き、拡張子はその右へ編集不可の文字として描く。
        ///       単に編集させないだけだと «最終的にどんなファイル名になるのか» が見えないため、
        ///       並べて出すことで固定されていることと結果の両方が一目で分かる。
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float extWidth = m_renameExtension.empty()
            ? 0.0f
            : ImGui::CalcTextSize(m_renameExtension.c_str()).x + spacing;
        /// @note グリッドのセル幅は可変なので、名前が 1 文字も打てない幅にならないよう下限を設ける。
        const float inputWidth = (std::max)(m_iconSize - extWidth, 40.0f);

        ImGui::SetNextItemWidth(inputWidth);
        /// @note バッファが拡張子を含まなくなったので、ImGui 既定の «フォーカス時に全選択» が
        ///       そのまま望みの挙動 (名前部分だけ選択) になる。手動の範囲指定は不要。
        if (m_renameNeedFocus) { ImGui::SetKeyboardFocusHere(); m_renameNeedFocus = false; }
        constexpr ImGuiInputTextFlags renameFlags = ImGuiInputTextFlags_EnterReturnsTrue;
        const bool enterPressed = ImGui::InputText("##rename", m_renameBuffer,
                                                   sizeof(m_renameBuffer), renameFlags);
        /// @note 直後に拡張子ラベルを描くと IsItemDeactivated() の対象がそちらへ移り、
        ///       «他所をクリックしてリネームを中断する» 経路が死ぬため、ここで確定させる。
        const bool inputDeactivated = ImGui::IsItemDeactivated();

        if (!m_renameExtension.empty()) {
            ImGui::SameLine(0.0f, spacing);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", m_renameExtension.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The extension is fixed: it identifies the asset type");
        }

        if (enterPressed) {
            if (m_renameBuffer[0] != '\0') {
                const std::string dir     = util::FileSystem::GetDirectory(e.path);
                const std::string newPath = dir + m_renameBuffer + m_renameExtension;
                if (newPath != e.path) {
                    auto doRename = [this, oldPath = e.path, newPath, context = &ctx]() {
                        /// @note .meta サイドカーも一緒に動かし、GUID 索引を追随させる。
                        if (!MoveAssetWithSidecar(oldPath, newPath)) {
                            FBZZ_LOG_ERROR("Rename failed: %s -> %s", oldPath.c_str(), newPath.c_str());
                        } else {
                            /// @note リネームも Undo 履歴には積まない (移動・生成・削除と同じ扱い)。
                            ///       ファイル名はディスクの状態であり、シーン編集の履歴に混ぜると
                            ///       Scene View の Ctrl+Z がアセットを勝手に改名することになる。
                            context->requestAssetBrowserRefresh = true;
                            if (m_selectedFbxPath == oldPath) m_selectedFbxPath = newPath;
                            ResetAssetPreviewCache(oldPath);
                            RefreshDirectory();
                        }
                    };
                    if (util::FileSystem::Exists(newPath)) {
                        ModalDialog::OpenConfirm("Rename",
                            "\"" + util::FileSystem::GetFilename(newPath) + "\" already exists. Overwrite?",
                            std::move(doRename));
                    } else {
                        doRename();
                    }
                }
            }
            m_renamingPath.clear();
            m_renameExtension.clear();
        } else if (inputDeactivated) {
            m_renamingPath.clear();
            m_renameExtension.clear();
        }
    } else {
        std::string display = e.name;
        while (display.size() > 2 &&
               ImGui::CalcTextSize(display.c_str()).x + ImGui::CalcTextSize("..").x > m_iconSize)
            display.pop_back();
        if (display.size() < e.name.size()) display += "..";

        const float indent = (m_iconSize - ImGui::CalcTextSize(display.c_str()).x) * 0.5f;
        if (indent > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        ImGui::TextUnformatted(display.c_str());

        if (!e.isMount && ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
            BeginRenameForPath(e.path, &ctx);
        }
    }
}

void AssetBrowserPanel::DrawEntry(const Entry& e, EditorContext& ctx, const SubAssetBand& band)
{
    ImGui::PushID(e.path.c_str());

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  sz     = m_iconSize;

    ImGui::InvisibleButton("##icon", { sz, sz });
    const bool hov = ImGui::IsItemHovered();

    /// @note 遅延リネーム: «選択済みをもう一度クリック» から一定時間で名前欄へ入る (Explorer 方式)。
    ///       この待ち時間は «開く» (ダブルクリック) と同じ操作の上に乗っており、2 度目の押下が
    ///       判定枠 (既定 0.30 秒) から遅れると開いたつもりが名前欄に入る (実際に発生した)。
    ///       カーソルが外れた・場所が動いた・ボタンが押された、のいずれかで名前欄へは入らない。
    if (!m_pendingRenamePath.empty() && m_pendingRenamePath == e.path) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float  dx    = mouse.x - m_pendingRenameMouse.x;
        const float  dy    = mouse.y - m_pendingRenameMouse.y;
        constexpr float kSlopPx = 4.0f;
        const bool moved = (dx * dx + dy * dy) > (kSlopPx * kSlopPx);

        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
            ImGui::IsMouseDown(ImGuiMouseButton_Left) || !hov || moved) {
            m_pendingRenamePath.clear();
        } else if (!e.isMount &&
                   static_cast<float>(ImGui::GetTime()) - m_pendingRenameTimer > 0.5f) {
            BeginRenameForPath(m_pendingRenamePath, &ctx);
            m_pendingRenamePath.clear();
        }
    }

    const bool primarySelected = !e.isDir && e.path == ctx.selectedAssetPath;
    const bool selected = primarySelected || m_selectedPaths.count(e.path) > 0;
    /// @note 複数選択中は「主選択 = Inspector に出ている 1 件」だけを濃く描き分ける。
    ///       単一選択のときは主選択かどうかを区別する意味がないので常に濃い表現にする。
    const bool multiSelection = m_selectedPaths.size() > 1;
    const bool emphasized     = primarySelected || !multiSelection;
    /// @note フォーカスを失っている間は彩度を落とす (Unity の Project ウィンドウと同じ)。
    const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 tileMin = { origin.x - 4.0f, origin.y - 4.0f };
    const ImVec2 tileMax = { origin.x + sz + 4.0f, origin.y + sz + 22.0f };

    /// @note サブアセット (FBX 内メッシュ / 画像内スプライト) と展開元の親を 1 本の帯で繋ぐ。
    ///       帯は隣接タイルとセル間の中点で接合するため、左右へ bleed だけ伸ばす。
    if (band.active) {
        ui::DrawSubAssetBand(
            dl,
            { tileMin.x - (band.joinLeft  ? band.bleed : 0.0f), tileMin.y },
            { tileMax.x + (band.joinRight ? band.bleed : 0.0f), tileMax.y },
            band.isParent, band.OpenLeft(), band.OpenRight(), 5.0f);
    }

    /// @name Unreal Content Browser 方式のカード
    /// @note 下地 → (色を付けたフォルダだけ) 色の面 → 選択ハイライト → サムネイル → 色の帯。
    ///       拡張子ごとの色を敷かないのは、色の意味を 1 系統に保つため — フォルダの色分けは
    ///       «自分で割り当てた分類» を表すが、種別でも色が付くと読み直しが要る。種別はサムネイルと
    ///       アイコン内のラベル (MAT / MESH …) が示すので、面の色は使わない。
    ImVec4 folderColor;
    const bool tinted  = e.isDir && TryGetFolderColor(ctx, e.path, folderColor);
    const float footerY = origin.y + sz;

    /// @note ホバーは «点く» のではなく «灯る»。タイルは一覧を舐めるように見るものなので、
    ///       一瞬で切り替わると視線の通り道が全部チカチカする。
    const float hoverT = widgets::Animate(ImGui::GetID("##tileHover"), hov ? 1.0f : 0.0f, 16.0f);

    /// @note サブアセットは親の帯に載っているので、カード下地は描かない。描くと帯とカードの
    ///       二重の面になり、親子のまとまりを示す帯が読めなくなる。
    if (!band.active)
        ui::DrawAssetTileCard(dl, tileMin, tileMax, hoverT, 5.0f);
    if (tinted)
        ui::DrawAssetTileTypeWash(dl, tileMin, tileMax, footerY, folderColor, 5.0f);

    ui::DrawTileSelection(dl, tileMin, tileMax, selected, hov, emphasized, panelFocused, 5.0f);

    /// @note Ping: 参照欄クリックで飛んできた対象を短時間だけ光らせる。選択ハイライトだけだと、
    ///       大量のタイルが並ぶ一覧の中で «今どれに飛ばされたのか» を目で拾えないため、
    ///       Unity の Project ウィンドウと同じく数百 ms のフラッシュで視線を誘導する。
    if (!m_pingPath.empty() && m_pingPath == e.path) {
        constexpr float PING_DURATION = 1.2f;
        const float elapsed = static_cast<float>(ImGui::GetTime()) - m_pingStartTime;
        if (elapsed < 0.0f || elapsed > PING_DURATION) {
            m_pingPath.clear();
        } else {
            /// @note 2 回明滅させてから消える。線形フェードだと「点いて消えた」だけで気づきにくい。
            const float phase = std::fabs(std::cos(elapsed * 6.2831853f));
            const float alpha = phase * (1.0f - elapsed / PING_DURATION);
            const ImVec4 accent = EditorTheme::Color(ThemeColor::Accent);
            dl->AddRect(tileMin, tileMax,
                        ImGui::GetColorU32({ accent.x, accent.y, accent.z, alpha }),
                        5.0f, 0, 2.5f);
        }
    }

    DrawAssetPreviewIconAt(origin, sz, e, ctx, hov);
    DrawEntryBadges(dl, origin, sz, e);

    /// @note 名前欄: 選択中は面で塗って白文字にし、サムネイルの絵柄に左右されず読めるようにする。
    if (selected && m_renamingPath != e.path) {
        ui::DrawTileLabelPlate(dl,
                               { tileMin.x + 2.0f, origin.y + sz + 4.0f },
                               { tileMax.x - 2.0f, tileMax.y - 1.0f },
                               emphasized, panelFocused, 4.0f);
    }

    /// @note 色を付けたフォルダの帯。選択の塗りと名前欄の下地の上に載せ、常に見えるようにする。
    if (tinted)
        ui::DrawAssetTileTypeStrip(dl, tileMin, tileMax, footerY, folderColor, 4.0f);

    /// @note ドラッグソース。フォルダも移動対象にし、左ペインのフォルダツリーへ直接整理できるようにする。
    if (!e.isMount && !e.isPackageAsset && ImGui::BeginDragDropSource()) {
        /// @note ドラッグ中はリリース時の選択変更を抑制
        m_entryDragStarted = true;
        PublishAssetDrag(e, ctx);
        ImGui::EndDragDropSource();
    }
    /// @note ドロップターゲット (ディレクトリのみ)
    if (e.isDir && ImGui::BeginDragDropTarget()) {
        /// @note SaveHierarchyPayloadAsPrefab が requestAssetBrowserRefresh を立てるため、ここで
        ///       RefreshDirectory() は呼ばない。DrawEntry の参照列を描画中に無効化してしまう。
        SaveHierarchyPayloadAsPrefab(
            ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, e.path);
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string sourcePath;
            if (ReadAssetDragPayload(p, sourcePath))
                QueueAssetMove(sourcePath, e.path);
        }
        ImGui::EndDragDropTarget();
    }

    if (hov && m_renamingPath != e.path) {
        if (e.isMount)
            ImGui::SetTooltip("%s\n\nExternal source folder mounted under Assets", e.path.c_str());
        else if (e.ext == ".fnt")
            ImGui::SetTooltip("%s\n\nStatic font atlas metadata\nDrag and drop onto UI Text Font Path to assign it", e.path.c_str());
        else if (e.ext == ".ttf" || e.ext == ".ttc" || e.ext == ".otf")
            ImGui::SetTooltip("%s\n\nFont file\nDrag and drop onto UI Text Font Path to assign it\nGlyphs are rasterized at runtime as they are used", e.path.c_str());
        else
            ImGui::SetTooltip("%s", e.path.c_str());
    }

    DrawEntryContextMenu(e, ctx);

    /// @note FBX / Sprite Texture の ▶/▼ 三角クリックで展開トグル。
    if (hov && !e.isSubAsset && e.hasSubAssets &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        /// @note 当たり判定はチップ (角丸の下地) の大きさに合わせる。見た目より狭いと
        ///       「押したのに開かない」が起きるため、DrawEntryBadges と同じ pad を使う。
        const float ts  = sz * 0.18f;
        const float pad = ts * 0.35f;
        const float bx  = origin.x + 2.0f - pad;
        const float by  = origin.y + sz - ts - 2.0f - pad;
        const float ext = ts + pad * 2.0f;
        const ImVec2 mp = ImGui::GetIO().MousePos;
        if (mp.x >= bx && mp.x <= bx + ext && mp.y >= by && mp.y <= by + ext) {
            if (m_expandedAssets.count(e.path))
                m_expandedAssets.erase(e.path);
            else
                m_expandedAssets.insert(e.path);
            m_assetExpandDirty = true;
            ImGui::PopID();
            return;
        }
    }
    HandleEntryClick(e, ctx, hov);
    HandleEntryDoubleClick(e, ctx, hov);
    {
        const bool tintLabel = selected && m_renamingPath != e.path;
        if (tintLabel) ImGui::PushStyleColor(ImGuiCol_Text, ui::TileSelectedTextColor());
        DrawEntryRenameLabel(e, ctx);
        if (tintLabel) ImGui::PopStyleColor();
    }

    /// @note FindRefs ポップアップは1つのエントリが最初にレンダリングされた後に開く
    if (m_findRefs.open) {
        ImGui::OpenPopup("##find_refs");
        m_findRefs.open = false;
    }
    DrawFindRefsPopup();

    ImGui::PopID();
}

void AssetBrowserPanel::DrawFindRefsPopup()
{
    ImGui::SetNextWindowSize({ 480.0f, 300.0f }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##find_refs", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) return;

    ImGui::TextUnformatted("Find References");
    ImGui::SameLine();
    ImGui::TextDisabled("— %s", util::FileSystem::GetFilename(m_findRefs.targetPath).c_str());
    ImGui::Separator();

    if (m_findRefs.results.empty()) {
        /// @note 「見つからない」を「使われていない」と読ませない。走査対象を必ず添える。
        ImGui::TextDisabled("(no references found)");
        ImGui::TextDisabled("scanned: .scene / .mat / .prefab / .animcontroller / .vfx");
        ImGui::TextDisabled("GUID 参照とパス参照の両方を検索しています。");
    } else {
        ImGui::TextDisabled("%zu file(s) reference this asset:", m_findRefs.results.size());
        ImGui::Spacing();
        const float avail = ImGui::GetContentRegionAvail().y - 34.0f;
        ImGui::BeginChild("##refs_list", { 0.0f, avail }, true);
        for (const auto& ref : m_findRefs.results) {
            const std::string label = util::FileSystem::GetFilename(ref);
            /// @note 別フォルダの同名ファイルがあるので、表示名ではなくフルパスで ID を分ける。
            ImGui::PushID(ref.c_str());
            if (ImGui::Selectable(label.c_str())) {
                /// @note クリックで親フォルダへナビゲート
                m_pendingNavigate = util::FileSystem::GetDirectory(ref);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ref.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::Separator();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

/// @name FBX 内容プレビュー (サブアセットアイコン)

} // namespace fbzz::editor
