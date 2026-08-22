// FBZZ Engine
// AssetBrowserItems.cpp | fbzz::editor
// AssetBrowser のフォルダツリーとファイルアイコン描画
#include "AssetBrowserCommon.hpp"
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/VFXEditorLauncher.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Windows.h>
#include <toml++/toml.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Util/Uuid.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

namespace fbzz::editor {
namespace {

// アセット本体と "<本体>.meta" サイドカーを一括で移動 / リネームし、GUID 索引を追随させる。
// WHY: .meta は AssetDatabase の恒久 guid を保持する。サイドカーを置き去りにすると
//      移動先で guid が再発行され、シーン / マテリアルからの guid: 参照が全て切れる。
//      ディレクトリ移動時は OnAssetMoved が配下の索引をプレフィックス付け替えで追随させる。
bool MoveAssetWithSidecar(const std::string& fromAbs, const std::string& toAbs)
{
    if (fromAbs.empty() || toAbs.empty() ||
        util::FileSystem::SamePathText(fromAbs, toAbs) ||
        !util::FileSystem::Exists(fromAbs) || util::FileSystem::Exists(toAbs))
        return false;

    const std::string fromMeta = fromAbs + ".meta";
    const std::string toMeta = toAbs + ".meta";
    const bool hasMeta = util::FileSystem::Exists(fromMeta);
    // 本体だけ移動して既存の .meta と結び付くと GUID の所有者が変わるため、先に拒否する。
    if (util::FileSystem::Exists(toMeta)) return false;

    if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(fromAbs),
                                  util::FileSystem::PathFromUtf8(toAbs)))
        return false;

    if (hasMeta) {
        if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(fromMeta),
                                      util::FileSystem::PathFromUtf8(toMeta))) {
            // サイドカーを移せない場合は本体を元へ戻し、半端な移動を残さない。
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

// 削除は Undo 履歴へ載せず、プロジェクト内のごみ箱へ退避する。
//
// WHY 履歴に載せないか (重要):
//   Undo スタックはシーン編集と共有されている。削除をそこへ積むと、Scene View で
//   Ctrl+Z / Ctrl+Y を押しただけでディスク上のファイルが復活したり再削除されたりする。
//   さらに旧実装は「Undo されないまま履歴からあふれたらデストラクタで退避データを完全削除」
//   していたため、履歴が 128 件を超えた瞬間に復元手段が予告なく消えていた。
//
// WHY ごみ箱へ移すか:
//   Undo 対象から外しても「消したものを取り戻せない」状態にはしたくない。
//   Unity の OS ごみ箱行きと同じ扱いで、実体は .fbzz/Trash/<日時>/ に残し続ける
//   (自動削除しない)。復元はエクスプローラーで戻すだけで済む。
// @return ごみ箱へ移せた項目数
std::size_t TrashAssets(const std::vector<std::string>& paths, EditorContext& ctx)
{
    if (paths.empty()) return 0;

    // 退避先は 1 回の削除操作につき 1 フォルダ。複数選択の削除をまとめて戻せるようにする。
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

        // .meta サイドカーも一緒に退避する。
        // WHY: 本体だけ消すと孤児 .meta が残る。ペアで移せば手で戻したときに guid も戻る。
        const std::string metaPath = paths[i] + ".meta";
        if (util::FileSystem::Exists(metaPath)) {
            util::FileSystem::Rename(util::FileSystem::PathFromUtf8(metaPath),
                                     util::FileSystem::PathFromUtf8(dest + ".meta"));
        }
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

// UniqueDuplicatePath は常に「元と同じフォルダ」に採番先を作る (その場複製用)。
// Ctrl+V は別フォルダへ貼り付けることが多いため、まず同名そのままを試し、
// 衝突する場合だけ "(2)" 以降を採番する。
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

bool CopyAssetPath(const std::string& srcPath, const std::string& dstPath, bool isDir)
{
    if (srcPath.empty() || dstPath.empty()) return false;
    return isDir
        ? util::FileSystem::CopyDirectoryRecursive(
            util::FileSystem::PathFromUtf8(srcPath),
            util::FileSystem::PathFromUtf8(dstPath))
        : util::FileSystem::CopyFile(
            util::FileSystem::PathFromUtf8(srcPath),
            util::FileSystem::PathFromUtf8(dstPath));
}

std::string ResolveMoveSourcePath(const std::string& payloadPath, const EditorContext& ctx)
{
    const std::string normalized = util::FileSystem::NormalizePathSeparators(payloadPath);
    if (normalized.empty()) return {};

    // 外部マウントの ASSET_PATH は絶対パスのまま渡されるため、projectRoot を二重付与しない。
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

    // 移動も Undo 履歴には積まない。
    // WHY: ファイルの場所はディスクの状態であって、シーン編集の履歴とは別の軸にある。
    //      同じスタックに載せると Scene View の Ctrl+Z がアセットを勝手に動かし、
    //      そのあいだに外部エディタや別操作が入ると復元先が実態と食い違う。
    ctx.requestAssetBrowserRefresh = true;

    outSrcAbs = srcAbs;
    outDstAbs = dstAbs;
    return true;
}

//      未知の拡張子は拡張子文字列のハッシュから色を生成し、
//      追加のコード変更なしにどんなファイルでも識別色が付く。
struct ExtGroup {
    const char*  exts[6];   // 最大 6 拡張子。nullptr 終番。
    ImVec4       color;
    const char*  label;
};

static constexpr ExtGroup kExtGroups[] = {
    { { ".hlsl", ".hlsli", nullptr },                          { 0.15f, 0.65f, 0.25f, 1.0f }, "HLSL"    },
    { { ".hpp", ".cpp", ".h", ".c", ".cc", ".cxx" },            { 0.20f, 0.58f, 0.70f, 1.0f }, "CPP"     },
    { { ".png", ".jpg", ".jpeg", ".dds", ".bmp", ".tga" },     { 0.15f, 0.40f, 0.80f, 1.0f }, "TEX"     },
    { { ".fbx", ".obj", ".gltf", ".glb", nullptr },            { 0.80f, 0.45f, 0.10f, 1.0f }, "MESH"    },
    { { ".prefab", nullptr },                              { 0.25f, 0.65f, 0.75f, 1.0f }, "PREFAB"  },
    { { ".terrain", nullptr },                             { 0.35f, 0.70f, 0.30f, 1.0f }, "TERRAIN" },
    { { ".scene", nullptr },                                    { 0.60f, 0.15f, 0.70f, 1.0f }, "SCENE"   },
    { { ".asset", nullptr },                                 { 0.85f, 0.55f, 0.08f, 1.0f }, "ASSET"   },
    { { ".anim", nullptr },                                  { 0.95f, 0.75f, 0.20f, 1.0f }, "ANIM"    },
    { { ".animcontroller", nullptr },                        { 0.75f, 0.40f, 0.85f, 1.0f }, "ANIM CTRL" },
    // Avatar Mask: アニメーションレイヤーの適用ボーン集合。Animator 系と同系色にする。
    { { ".mask", nullptr },                                  { 0.55f, 0.45f, 0.90f, 1.0f }, "MASK"    },
    { { ".vfx", nullptr },                                   { 0.95f, 0.35f, 0.55f, 1.0f }, "VFX"     },
    { { ".behaviortree", nullptr },                          { 0.45f, 0.80f, 0.65f, 1.0f }, "AI"      },
    { { ".mat", nullptr },                                   { 0.20f, 0.70f, 0.80f, 1.0f }, "MAT"     },
    // 物理マテリアル。見た目の .mat と取り違えないよう、色は物理系 (青緑) から離す。
    { { ".physmat", nullptr },                               { 0.90f, 0.50f, 0.25f, 1.0f }, "PHYSMAT" },
    { { ".tex", nullptr },                                   { 0.40f, 0.80f, 0.90f, 1.0f }, "TEX"     },
    { { ".mesh", nullptr },                                  { 0.80f, 0.45f, 0.10f, 1.0f }, "MESH"    },
    { { ".animctrl", nullptr },                              { 0.35f, 0.75f, 0.45f, 1.0f }, "CTRL"    },
    { { ".toml", ".json", ".yaml", ".yml", nullptr },           { 0.65f, 0.65f, 0.10f, 1.0f }, "DATA"    },
    { { ".wav", ".mp3", ".ogg", ".flac", nullptr },             { 0.70f, 0.20f, 0.50f, 1.0f }, "SFX"     },
    { { ".ttf", ".ttc", ".otf", nullptr },                     { 0.60f, 0.30f, 0.85f, 1.0f }, "FONT"    },
    { { ".fnt", nullptr },                                     { 0.50f, 0.20f, 0.75f, 1.0f }, "FNT"     },
    { { ".txt", ".md", ".rst", nullptr },                      { 0.55f, 0.55f, 0.55f, 1.0f }, "TEXT"    },
    { { ".py", ".lua", ".cs", nullptr },                       { 0.20f, 0.70f, 0.55f, 1.0f }, "SCRIPT"  },
};

// 未知拡張子をハッシュで色付けする。
// WHAT: FNV-1a の下位ビットを色相に変換し、彩度・明度は固定で
//       読みやすい明るさに調整する。同じ拡張子なら常に同じ色になる。
static ImVec4 ColorFromExt(const std::string& ext)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : ext)
        h = (h ^ c) * 16777619u;
    const float hue = static_cast<float>(h & 0xFFFF) / 65536.0f; // 0..1
    // HSV → RGB (S=0.55, V=0.72)
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

// 拡張子がグループに含まれるか確認する。
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
    // .dds はキューブマップ等の非 2D テクスチャを含むため 2D プレビュー対象から除外する
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".bmp" || ext == ".tga";
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

    // WHY: .mat 内のテクスチャ参照は Assets/ 相対で保存されるため、
    //      ResourceManager が読める実ファイルパスに変換してからサムネイルを読み込む。
    if (normalized.starts_with("Assets/") && !projectRoot.empty()) {
        return projectRoot + "/" + normalized;
    }
    return normalized;
}

// FBX の従属アセットを Library/Baked/<guid> から解決する。
// WHY: インポーターが生成する .fzasset / .mat の正規配置を一箇所に固定し、
//      AssetBrowser が原本 FBX 隣の中間生成物へ依存しないようにする。
static std::filesystem::path ResolveModelGeneratedDir(const std::string& sourcePath,
                                                       const char* generatedName)
{
    const std::string bakedDir = asset::AssetManager::BakedDirForSource(sourcePath);
    if (bakedDir.empty()) return {};
    return util::FileSystem::PathFromUtf8(bakedDir) / generatedName;
}

static ImTextureID ToImTextureID(void* ptr)
{
    // WHY: このプロジェクトの ImGui は ImTextureID を ImU64 として扱う。
    //      void* のビット列を整数 ID に移すだけなので、所有権や型変換の意味を持たせない。
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

static std::string SelectMaterialPreviewTexture(const asset::MaterialAsset& mat)
{
    static constexpr const char* PRIORITY_SLOTS[] = {
        "albedo", "base_color", "diffuse", "layer0_diffuse", "foamTex"
    };
    for (const char* slot : PRIORITY_SLOTS) {
        if (auto it = mat.textures.find(slot); it != mat.textures.end() && !it->second.empty()) {
            return it->second;
        }
    }
    for (const auto& [slot, path] : mat.textures) {
        if (!path.empty()) return path;
    }
    return {};
}

static ImVec4 SelectMaterialColor(const asset::MaterialAsset& mat)
{
    const auto findColor = [&mat]() -> const std::vector<float>* {
        if (auto it = mat.params.find("base_color"); it != mat.params.end() && it->second.size() >= 3) return &it->second;
        if (auto it = mat.params.find("baseColor"); it != mat.params.end() && it->second.size() >= 3) return &it->second;
        if (auto it = mat.params.find("albedo"); it != mat.params.end() && it->second.size() >= 3) return &it->second;
        return nullptr;
    };

    if (const std::vector<float>* values = findColor()) {
        const float alpha = values->size() >= 4 ? (*values)[3] : 1.0f;
        return { (*values)[0], (*values)[1], (*values)[2], alpha };
    }
    return { 0.20f, 0.70f, 0.80f, 1.0f };
}

enum class ThumbnailShaderFlavor {
    Surface,
    Skinned,
    Terrain,
    Water,
    Unsupported,
};

static std::string ToLowerAssetPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
}

static ThumbnailShaderFlavor DetectThumbnailShaderFlavor(std::string_view shaderPath)
{
    const std::string lower = ToLowerAssetPath(std::string(shaderPath));
    if (lower.find("/material/effects/") != std::string::npos) return ThumbnailShaderFlavor::Unsupported;
    if (lower.find("/effects/particle") != std::string::npos) return ThumbnailShaderFlavor::Unsupported;
    if (lower.find("/effects/trail") != std::string::npos) return ThumbnailShaderFlavor::Unsupported;
    if (lower.find("/effects/meshtrail") != std::string::npos) return ThumbnailShaderFlavor::Unsupported;
    if (lower.find("/water/") != std::string::npos) return ThumbnailShaderFlavor::Water;
    if (lower.find("/terrain/") != std::string::npos) return ThumbnailShaderFlavor::Terrain;
    if (lower.find("/material/skinned/") != std::string::npos) return ThumbnailShaderFlavor::Skinned;
    return ThumbnailShaderFlavor::Surface;
}

static ThumbnailShaderFlavor DetectMaterialThumbnailFlavor(const asset::MaterialAsset& asset)
{
    // WHY: Particle / Trail 用 .mat は MeshRenderer と頂点入力・定数バッファが違うため、
    //      AssetBrowser の球メッシュ preview に流すと不正な IA レイアウトでクラッシュし得る。
    //      mesh_type / render_path を .mat の信頼元として扱い、shader path だけの推測を避ける。
    // UI 用 .mat も同じ理由で弾く。UI パスは b0 を UIConstants として使うため、
    // 球メッシュのプレビューでは ortho 行列の位置にカメラ行列が入り、頂点が飛ぶ。
    // Decal 用 .mat は頂点入力そのものを持たない (SV_VertexID でフルスクリーン三角形)。
    if (asset.renderPath == asset::RenderPath::Particle ||
        asset.renderPath == asset::RenderPath::Trail ||
        asset.renderPath == asset::RenderPath::UI ||
        asset.renderPath == asset::RenderPath::Decal) {
        return ThumbnailShaderFlavor::Unsupported;
    }
    if (asset.meshType == asset::MeshType::Skinned) {
        return ThumbnailShaderFlavor::Skinned;
    }
    return DetectThumbnailShaderFlavor(asset.shaderPath);
}

// t0-t15 は標準 Material スロット。Terrain/Water は専用名で解決されるが、
// それ以外の汎用スロットはここで名前フォールバックが効く。
// WHY: preview.textures.assign(16,{}) と同サイズにすることで、
//      bind.slot が 8-15 の汎用スロットでも FindMaterialTexturePath が機能する。
constexpr std::array<const char*, 16> kMaterialTextureSlotNames = {
    "albedo",
    "normal",
    "metallic",
    "emissive",
    "ao",
    "tex5",
    "tex6",
    "tex7",
    "tex8",
    "tex9",
    "tex10",
    "tex11",
    "tex12",
    "tex13",
    "tex14",
    "tex15",
};

static const std::vector<float>* FindMaterialParam(const asset::MaterialAsset& asset, std::string_view shaderVarName)
{
    auto it = asset.params.find(std::string(shaderVarName));
    if (it != asset.params.end()) return &it->second;

    // WHY: .mat は PBR 寄りの名前、HLSL は shader ごとの短い変数名を使う場合がある。
    //      サムネイルも本編描画と同じ別名吸収を行い、shaderPath を変えても色や係数を反映する。
    if (shaderVarName == "albedo")              it = asset.params.find("base_color");
    else if (shaderVarName == "metallic")       it = asset.params.find("metallic_factor");
    else if (shaderVarName == "roughness")      it = asset.params.find("roughness_factor");
    else if (shaderVarName == "normalStrength") it = asset.params.find("normal_strength");
    else if (shaderVarName == "emissiveColor")  it = asset.params.find("emissive_color");
    else if (shaderVarName == "emissiveScale")  it = asset.params.find("emissive_scale");

    return it != asset.params.end() ? &it->second : nullptr;
}

static float MaterialParamFloat(const asset::MaterialAsset* asset, std::string_view name, float fallback)
{
    if (!asset) return fallback;
    const auto* values = FindMaterialParam(*asset, name);
    return (values && !values->empty()) ? (*values)[0] : fallback;
}

static math::Vector3 MaterialParamFloat3(const asset::MaterialAsset* asset, std::string_view name, math::Vector3 fallback)
{
    if (!asset) return fallback;
    const auto* values = FindMaterialParam(*asset, name);
    if (!values || values->size() < 3) return fallback;
    return { (*values)[0], (*values)[1], (*values)[2] };
}

static void InitDefaultMaterialParams(const renderer::ShaderDescriptor& desc, std::vector<uint8_t>& paramData)
{
    const float one = 1.0f;
    for (const auto& v : desc.vars) {
        if (v.varType != renderer::ShaderVarType::Float) continue;
        for (uint32_t col = 0; col < v.columns; ++col) {
            const uint32_t byteOff = v.offset + col * sizeof(float);
            if (byteOff + sizeof(float) <= static_cast<uint32_t>(paramData.size()))
                std::memcpy(paramData.data() + byteOff, &one, sizeof(float));
        }
    }

    auto setFloat = [&](std::string_view name, float value) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns != 1) return;
        if (v->offset + sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, &value, sizeof(float));
    };
    auto setFloat2 = [&](std::string_view name, const float value[2]) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 2) return;
        if (v->offset + 2u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 2u * sizeof(float));
    };
    auto setFloat3 = [&](std::string_view name, const float value[3]) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 3) return;
        if (v->offset + 3u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 3u * sizeof(float));
    };

    const float uvTiling[2] = { 1.0f, 1.0f };
    const float uvOffset[2] = { 0.0f, 0.0f };
    const float white3[3] = { 1.0f, 1.0f, 1.0f };
    setFloat("metallic", 0.0f);
    setFloat("roughness", 0.65f);
    setFloat("emissiveScale", 0.0f);
    setFloat("alphaCutoff", 0.5f);
    setFloat2("uvTiling", uvTiling);
    setFloat2("uvOffset", uvOffset);
    setFloat3("emissiveColor", white3);
}

static void ApplyMaterialAssetParams(const asset::MaterialAsset& asset,
                                     const renderer::ShaderDescriptor& desc,
                                     std::vector<uint8_t>& paramData)
{
    for (const auto& v : desc.vars) {
        if (v.varType != renderer::ShaderVarType::Float) continue;
        if (v.offset + v.size > static_cast<uint32_t>(paramData.size())) continue;

        const auto* values = FindMaterialParam(asset, v.name);
        if (!values || values->empty()) continue;

        const size_t count = std::min<size_t>(v.columns, values->size());
        std::memcpy(paramData.data() + v.offset, values->data(), count * sizeof(float));
    }
}

static std::string FindMaterialTexturePath(const asset::MaterialAsset& asset,
                                           const renderer::ShaderTexBindDesc& bind)
{
    auto byShaderName = asset.textures.find(bind.name);
    if (byShaderName != asset.textures.end()) return byShaderName->second;

    const std::string lowerName = ToLowerAssetPath(bind.name);
    const auto findTexture = [&asset](const char* name) -> std::string {
        auto it = asset.textures.find(name);
        return it != asset.textures.end() ? it->second : std::string{};
    };
    if (lowerName == "g_normalmap1") return findTexture("normalMap1");
    if (lowerName == "g_normalmap2") return findTexture("normalMap2");
    if (lowerName == "g_foamtex")    return findTexture("foamTex");
    if (lowerName == "g_foammask")   return findTexture("foamMask");
    if (lowerName == "g_envtex")     return findTexture("envCubemap");
    if (lowerName == "g_flowmap")    return findTexture("flowMap");
    if (lowerName == "g_splatmap")   return findTexture("splatmap");
    if (lowerName.starts_with("g_diffuse") && bind.slot >= 1 && bind.slot <= 4) {
        const std::string slot = "layer" + std::to_string(bind.slot - 1) + "_diffuse";
        auto it = asset.textures.find(slot);
        return it != asset.textures.end() ? it->second : std::string{};
    }
    if (lowerName.starts_with("g_normal") && bind.slot >= 5 && bind.slot <= 8) {
        const std::string slot = "layer" + std::to_string(bind.slot - 5) + "_normal";
        auto it = asset.textures.find(slot);
        return it != asset.textures.end() ? it->second : std::string{};
    }
    if (lowerName.starts_with("g_aoroughness") && bind.slot >= 9 && bind.slot <= 12) {
        const std::string slot = "layer" + std::to_string(bind.slot - 9) + "_ao_roughness";
        auto it = asset.textures.find(slot);
        return it != asset.textures.end() ? it->second : std::string{};
    }

    if (bind.slot < kMaterialTextureSlotNames.size()) {
        auto byStandardName = asset.textures.find(kMaterialTextureSlotNames[bind.slot]);
        if (byStandardName != asset.textures.end()) return byStandardName->second;

        // Terrain レイヤーの fzmat はプレフィックスなし ("diffuse", "ao_roughness") を使う。
        // WHY: Fallback/Surface シェーダーが slot 0="albedo", slot 4="ao" を期待するが、
        //      レイヤー fzmat にはこれらが存在しないため別名でフォールバックする。
        const std::string_view standard = kMaterialTextureSlotNames[bind.slot];
        if (standard == "albedo") {
            auto it = asset.textures.find("diffuse");
            if (it != asset.textures.end() && !it->second.empty()) return it->second;
        }
        if (standard == "ao") {
            auto it = asset.textures.find("ao_roughness");
            if (it != asset.textures.end() && !it->second.empty()) return it->second;
        }
    }
    return {};
}

} // namespace

bool AssetBrowserPanel::RebuildMaterialThumbnailGpuData(MaterialPreview& preview, EditorContext& ctx)
{
    if (!ctx.resources) return false;

    const bool useFallbackMaterial = preview.asset.shaderPath.empty();
    asset::MaterialAsset fallbackAsset;
    const asset::MaterialAsset* renderAsset = &preview.asset;
    if (useFallbackMaterial) {
        fallbackAsset.shaderPath = "Assets/Shaders/Material/Surface/Fallback.hlsl";
        fallbackAsset.params["albedo"] = { 1.0f, 0.0f, 1.0f, 1.0f };
        renderAsset = &fallbackAsset;
    }

    const std::string nextShaderPath = renderAsset->shaderPath.empty()
        ? "Assets/Shaders/Material/Surface/Fallback.hlsl"
        : renderAsset->shaderPath;
    if (nextShaderPath != preview.shaderPath) {
        preview.shaderPath = nextShaderPath;
        preview.shader = {};
        if (ctx.resources && preview.materialCB.IsValid()) {
            ctx.resources->Release(preview.materialCB);
            preview.materialCB = {};
        }
        preview.textures.clear();
        preview.paramData.clear();
    }

    if (!preview.shader.IsValid())
        preview.shader = ctx.resources->LoadShader(preview.shaderPath);
    auto* shader = ctx.resources->Get(preview.shader);
    if (!shader) return false;

    const renderer::ShaderDescriptor& desc = shader->GetDescriptor();
    const ThumbnailShaderFlavor flavor = DetectMaterialThumbnailFlavor(*renderAsset);
    if (flavor == ThumbnailShaderFlavor::Unsupported) return false;
    if (!desc.IsValid() && flavor != ThumbnailShaderFlavor::Terrain) return false;

    preview.textures.assign(16, {});
    if (!desc.textures.empty()) {
        for (const auto& bind : desc.textures) {
            if (bind.slot >= preview.textures.size()) continue;
            const std::string texturePath = FindMaterialTexturePath(*renderAsset, bind);
            if (!texturePath.empty())
                preview.textures[bind.slot] = ctx.resources->LoadTexture(ToTextureLoadPath(texturePath, ctx));
        }
    }
    if (flavor == ThumbnailShaderFlavor::Terrain) {
        const auto loadSlot = [&](uint32_t slot, const char* name) {
            auto it = renderAsset->textures.find(name);
            if (it != renderAsset->textures.end() && !it->second.empty())
                preview.textures[slot] = ctx.resources->LoadTexture(ToTextureLoadPath(it->second, ctx));
        };
        loadSlot(0, "splatmap");
        for (uint32_t layer = 0; layer < 4; ++layer) {
            const std::string prefix = "layer" + std::to_string(layer);
            loadSlot(1 + layer, (prefix + "_diffuse").c_str());
            loadSlot(5 + layer, (prefix + "_normal").c_str());
            loadSlot(9 + layer, (prefix + "_ao_roughness").c_str());
        }
    }
    if (!desc.IsValid()) {
        preview.materialCB = {};
        preview.paramData.clear();
        return true;
    }

    preview.paramData.assign(desc.cbufferSize, 0u);
    InitDefaultMaterialParams(desc, preview.paramData);
    ApplyMaterialAssetParams(*renderAsset, desc, preview.paramData);

    if (desc.textureMaskOffset != UINT32_MAX &&
        desc.textureMaskOffset + sizeof(uint32_t) <= preview.paramData.size()) {
        uint32_t mask = 0;
        for (const auto& bind : desc.textures) {
            if (bind.slot >= preview.textures.size()) continue;
            const std::string texturePath = FindMaterialTexturePath(*renderAsset, bind);
            if (texturePath.empty()) continue;
            preview.textures[bind.slot] = ctx.resources->LoadTexture(ToTextureLoadPath(texturePath, ctx));
            if (preview.textures[bind.slot].IsValid() && bind.slot < 8)
                mask |= (1u << bind.slot);
        }
        std::memcpy(preview.paramData.data() + desc.textureMaskOffset, &mask, sizeof(uint32_t));
    } else {
        for (const auto& bind : desc.textures) {
            if (bind.slot >= preview.textures.size()) continue;
            const std::string texturePath = FindMaterialTexturePath(*renderAsset, bind);
            if (!texturePath.empty())
                preview.textures[bind.slot] = ctx.resources->LoadTexture(ToTextureLoadPath(texturePath, ctx));
        }
    }

    if (!preview.materialCB.IsValid())
        preview.materialCB = ctx.resources->CreateConstantBuffer(desc.cbufferSize);
    if (!preview.materialCB.IsValid()) return false;
    ctx.resources->Update(preview.materialCB, preview.paramData.data(), preview.paramData.size());
    return true;
}

namespace {

// サムネイルレンダリング用の共有 GPU リソースをまとめて保持する構造体。
// WHY: 以前は RenderMeshThumbnail / DrawAssetPreviewIconAt の function-local static
//      として暗黙的に共有されていた。構造体に昇格させて意図を明示する。
struct ThumbnailRenderer {
    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> terrainObjectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> waterObjectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB;
    renderer::ResourceHandle<renderer::TextureTag>        whiteTexture;
    renderer::ResourceHandle<renderer::TextureTag>        blackTexture;
    renderer::ResourceHandle<renderer::TextureTag>        flatNormalTexture;
    renderer::Mesh*                                        materialSphere = nullptr;
    renderer::Mesh*                                        skinnedMaterialSphere = nullptr;
    renderer::Mesh*                                        waterMaterialSphere = nullptr;
};
static ThumbnailRenderer s_tr;

static void DrawThumbnailFrame(ImVec2 origin, float sz, bool hovered)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = hovered ? IM_COL32(42, 45, 52, 255) : IM_COL32(30, 32, 38, 255);
    dl->AddRectFilled(origin, { origin.x + sz, origin.y + sz }, bg, 4.0f);
    // 上明→下暗の縦グラデーションで背景に奥行きを持たせ、プレビューの立体感を補強する。
    // WHY: AddRectFilledMultiColor は角丸非対応のため、角丸 (4px) の欠けが届かない
    //      2px 内側へ矩形で重ね、ベースの角丸輪郭を保つ。
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

// Sprite Texture の元素材は atlas 全体、仮想サブアセットは個別 SpriteRect を切り抜いて表示する。
// WHY: 元画像と個々の Sprite の見た目を同時に比較できる Unity 風の展開表示にする。
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

struct ThumbnailMaterialCB {
    math::Vector4 albedo = math::Vector4::WHITE;
    uint32_t textureMask = 0;
    float _pad[3] = {};
};

struct ThumbnailTerrainCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    math::Vector4 layerTiling[4];
    math::Vector4 layerNormalStrength;
    math::Vector4 layerMaterial[4];
    math::Vector4 layerTextureFlags;
    math::Vector4 layerAutoHeight[4];
    math::Vector4 layerAutoSlope[4];
};

// Assets/Shaders/Water/Water.hlsl の WaterCB と一致させる
// (実体は WaterRenderPass.cpp の WaterCB。レイアウトを変えたら両方直すこと)。
struct ThumbnailWaterCB {
    math::Matrix4 worldMatrix;
    math::Matrix4 wvpMatrix;
    math::Vector4 shallowColorDepth;
    math::Vector4 deepColorDepth;
    math::Vector4 surfaceParams;
    math::Vector4 normalParams;
    math::Vector4 timeParams;
    math::Vector4 foamParams;
    math::Vector4 refractionFlowParams;
    math::Vector4 waveDir[4];
    math::Vector4 waveParams[4];
    math::Vector4 detailParams;
    math::Vector4 sssParams;
    math::Vector4 reflectParams;
    math::Vector4 flowParams;
};

struct ThumbnailSkinningCB {
    math::Matrix4 boneMatrices[128];
};

struct ThumbnailWaterVertex {
    math::Vector3 position;
    math::Vector2 uv;
};

static renderer::Mesh* CreateSkinnedPreviewSphere(renderer::ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<renderer::Mesh>> s_cache;
    if (auto it = s_cache.find(segments); it != s_cache.end()) return it->second.get();

    auto* surface = renderer::PrimitiveMesh::Sphere(resources, segments);
    if (!surface) return nullptr;

    std::vector<renderer::SkinnedVertex> verts;
    verts.reserve(surface->cpuVertices.size());
    for (const auto& v : surface->cpuVertices) {
        renderer::SkinnedVertex sv{};
        sv.position = v.position;
        sv.normal = v.normal;
        sv.tangent = v.tangent;
        sv.uv = v.uv;
        sv.boneIndices[0] = 0;
        sv.boneWeights[0] = 1.0f;
        verts.push_back(sv);
    }

    auto mesh = std::shared_ptr<renderer::Mesh>(new renderer::Mesh());
    mesh->vertexBuffer = resources.CreateVertexBuffer(verts.data(), verts.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(surface->cpuIndices.data(), static_cast<uint32_t>(surface->cpuIndices.size()));
    mesh->vertexCount = static_cast<uint32_t>(verts.size());
    mesh->indexCount = static_cast<uint32_t>(surface->cpuIndices.size());
    mesh->isSkinned = true;
    mesh->cpuSkinnedVertices = std::move(verts);
    mesh->cpuIndices = surface->cpuIndices;
    mesh->ComputeBounds();
    s_cache[segments] = mesh;
    return mesh.get();
}

static renderer::Mesh* CreateWaterPreviewSphere(renderer::ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<renderer::Mesh>> s_cache;
    if (auto it = s_cache.find(segments); it != s_cache.end()) return it->second.get();

    auto* surface = renderer::PrimitiveMesh::Sphere(resources, segments);
    if (!surface) return nullptr;

    std::vector<ThumbnailWaterVertex> verts;
    verts.reserve(surface->cpuVertices.size());
    for (const auto& v : surface->cpuVertices)
        verts.push_back({ v.position, v.uv });

    auto mesh = std::shared_ptr<renderer::Mesh>(new renderer::Mesh());
    mesh->vertexBuffer = resources.CreateVertexBuffer(verts.data(), verts.size() * sizeof(ThumbnailWaterVertex), sizeof(ThumbnailWaterVertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(surface->cpuIndices.data(), static_cast<uint32_t>(surface->cpuIndices.size()));
    mesh->vertexCount = static_cast<uint32_t>(verts.size());
    mesh->indexCount = static_cast<uint32_t>(surface->cpuIndices.size());
    mesh->cpuVertices = surface->cpuVertices;
    mesh->cpuIndices = surface->cpuIndices;
    mesh->ComputeBounds();
    s_cache[segments] = mesh;
    return mesh.get();
}

static math::Vector3 MeshBoundsCenter(const renderer::Mesh& mesh)
{
    if (mesh.boundsRadius > 0.0f) return mesh.boundsCenter;

    math::Vector3 minP{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    math::Vector3 maxP{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()
    };
    auto visit = [&](const math::Vector3& p) {
        minP.x = std::min(minP.x, p.x);
        minP.y = std::min(minP.y, p.y);
        minP.z = std::min(minP.z, p.z);
        maxP.x = std::max(maxP.x, p.x);
        maxP.y = std::max(maxP.y, p.y);
        maxP.z = std::max(maxP.z, p.z);
    };
    if (mesh.isSkinned) {
        for (const auto& v : mesh.cpuSkinnedVertices) visit(v.position);
    } else {
        for (const auto& v : mesh.cpuVertices) visit(v.position);
    }
    if (minP.x > maxP.x) return {};
    return (minP + maxP) * 0.5f;
}

static float MeshBoundsRadius(const renderer::Mesh& mesh, const math::Vector3& center)
{
    if (mesh.boundsRadius > 0.0f) return mesh.boundsRadius;

    float radiusSq = 0.0f;
    auto visit = [&](const math::Vector3& p) {
        radiusSq = std::max(radiusSq, (p - center).LengthSq());
    };
    if (mesh.isSkinned) {
        for (const auto& v : mesh.cpuSkinnedVertices) visit(v.position);
    } else {
        for (const auto& v : mesh.cpuVertices) visit(v.position);
    }
    return std::sqrt(std::max(radiusSq, 0.0001f));
}

static bool EnsureThumbnailDefaultTextures(renderer::ResourceManager& resources,
                                           renderer::ResourceHandle<renderer::TextureTag>& white,
                                           renderer::ResourceHandle<renderer::TextureTag>& black,
                                           renderer::ResourceHandle<renderer::TextureTag>& flatNormal)
{
    if (!white.IsValid()) {
        const uint8_t rgba[4] = { 255, 255, 255, 255 };
        white = resources.CreateTexture(rgba, 1, 1);
    }
    if (!black.IsValid()) {
        const uint8_t rgba[4] = { 0, 0, 0, 255 };
        black = resources.CreateTexture(rgba, 1, 1);
    }
    if (!flatNormal.IsValid()) {
        const uint8_t rgba[4] = { 128, 128, 255, 255 };
        flatNormal = resources.CreateTexture(rgba, 1, 1);
    }
    return white.IsValid() && black.IsValid() && flatNormal.IsValid();
}

static renderer::ResourceHandle<renderer::TextureTag> DefaultThumbnailTextureForSlot(
    ThumbnailShaderFlavor flavor,
    uint32_t slot,
    renderer::ResourceHandle<renderer::TextureTag> white,
    renderer::ResourceHandle<renderer::TextureTag> black,
    renderer::ResourceHandle<renderer::TextureTag> flatNormal)
{
    if (flavor == ThumbnailShaderFlavor::Water) {
        if (slot == 0 || slot == 1 || slot == 7) return flatNormal;
        if (slot == 4 || slot == 6 || slot == 8) return black;
        return white;
    }
    if (flavor == ThumbnailShaderFlavor::Terrain) {
        if (slot >= 5 && slot <= 8) return flatNormal;
        return white;
    }
    return {};
}

static ThumbnailTerrainCB BuildThumbnailTerrainCB(
    const asset::MaterialAsset* asset,
    const math::Matrix4& viewProjection)
{
    ThumbnailTerrainCB cb{};
    cb.worldMatrix = math::Matrix4::Identity();
    cb.wvpMatrix = viewProjection;
    for (int i = 0; i < 4; ++i) {
        const std::string prefix = "layer" + std::to_string(i) + "_";
        cb.layerTiling[i] = {
            MaterialParamFloat(asset, prefix + "tilingX", 2.0f),
            MaterialParamFloat(asset, prefix + "tilingZ", 2.0f),
            0.0f,
            0.0f
        };
        const float normalStrength = MaterialParamFloat(asset, prefix + "normalStrength", 1.0f);
        if (i == 0) cb.layerNormalStrength.x = normalStrength;
        else if (i == 1) cb.layerNormalStrength.y = normalStrength;
        else if (i == 2) cb.layerNormalStrength.z = normalStrength;
        else cb.layerNormalStrength.w = normalStrength;
        cb.layerMaterial[i] = {
            MaterialParamFloat(asset, prefix + "roughness", 0.8f),
            MaterialParamFloat(asset, prefix + "ambientOcclusion", 1.0f),
            0.0f,
            0.0f
        };
        cb.layerAutoHeight[i] = {
            MaterialParamFloat(asset, prefix + "autoMinHeight", -10000.0f),
            MaterialParamFloat(asset, prefix + "autoMaxHeight", 10000.0f),
            MaterialParamFloat(asset, prefix + "autoHeightFade", 1.0f),
            MaterialParamFloat(asset, prefix + "autoBlendEnabled", 0.0f)
        };
        cb.layerAutoSlope[i] = {
            MaterialParamFloat(asset, prefix + "autoMinSlope", 0.0f),
            MaterialParamFloat(asset, prefix + "autoMaxSlope", 1.0f),
            MaterialParamFloat(asset, prefix + "autoSlopeFade", 0.1f),
            MaterialParamFloat(asset, prefix + "autoBlendStrength", 1.0f)
        };
    }
    return cb;
}

static ThumbnailWaterCB BuildThumbnailWaterCB(
    const asset::MaterialAsset* asset,
    const math::Matrix4& viewProjection)
{
    ThumbnailWaterCB cb{};
    cb.worldMatrix = math::Matrix4::Identity();
    cb.wvpMatrix = viewProjection;
    const math::Vector3 shallow = MaterialParamFloat3(asset, "shallowColor", { 0.20f, 0.60f, 0.70f });
    const math::Vector3 deep = MaterialParamFloat3(asset, "deepColor", { 0.00f, 0.10f, 0.30f });
    cb.shallowColorDepth = { shallow.x, shallow.y, shallow.z, MaterialParamFloat(asset, "shallowDepth", 0.5f) };
    cb.deepColorDepth = { deep.x, deep.y, deep.z, MaterialParamFloat(asset, "deepDepth", 5.0f) };
    cb.surfaceParams = {
        MaterialParamFloat(asset, "opacity", 0.85f),
        MaterialParamFloat(asset, "reflectivity", 0.35f),
        MaterialParamFloat(asset, "fresnelBias", 0.02f),
        MaterialParamFloat(asset, "fresnelPower", 5.0f)
    };
    cb.normalParams = { 0.0f, 0.0f, 0.0f, MaterialParamFloat(asset, "normalStrength", 0.75f) };
    cb.timeParams   = { 0.0f, 0.0f, 0.0f, 0.35f };
    cb.foamParams = {
        MaterialParamFloat(asset, "foamThreshold", 0.3f),
        MaterialParamFloat(asset, "foamFade", 0.5f),
        MaterialParamFloat(asset, "foamStrength", 0.6f),
        MaterialParamFloat(asset, "foamNoiseScale", 0.5f)
    };
    cb.refractionFlowParams = {
        MaterialParamFloat(asset, "refractionStrength", 0.02f),
        MaterialParamFloat(asset, "flowSpeed", 0.3f),
        0.0f,
        0.0f
    };
    cb.detailParams = {
        MaterialParamFloat(asset, "detailScale", 0.35f),
        MaterialParamFloat(asset, "detailSpeed", 0.6f),
        MaterialParamFloat(asset, "detailStrength", 1.0f),
        MaterialParamFloat(asset, "smoothness", 0.92f)
    };
    const math::Vector3 sss = MaterialParamFloat3(asset, "sssColor", { 0.12f, 0.50f, 0.46f });
    cb.sssParams = { sss.x, sss.y, sss.z, MaterialParamFloat(asset, "sssStrength", 0.6f) };
    // サムネイルは IBL キューブを持たないので、空反射はフラット色へフォールバックさせる。
    cb.reflectParams = { 0.0f, 0.0f, 0.0f, 0.35f };
    cb.flowParams    = { 1.0f, 0.0f, 0.0f, 0.0f };
    return cb;
}

static bool EnsureThumbnailGpuResources(renderer::ResourceManager& resources,
                                         ThumbnailRenderer& tr,
                                         bool requireFallbackMaterial)
{
    if (requireFallbackMaterial && !tr.shader.IsValid())
        tr.shader = resources.LoadShader("Assets/Shaders/Material/Surface/Lit.hlsl");
    if (requireFallbackMaterial && !tr.shader.IsValid()) return false;

    if (!tr.pso.IsValid()) {
        tr.pso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_ON
        });
    }
    if (!tr.frameCB.IsValid())
        tr.frameCB = resources.CreateConstantBuffer(sizeof(scene::PerFrameCB));
    if (!tr.objectCB.IsValid())
        tr.objectCB = resources.CreateConstantBuffer(sizeof(scene::PerObjectCB));
    if (requireFallbackMaterial && !tr.materialCB.IsValid())
        tr.materialCB = resources.CreateConstantBuffer(sizeof(ThumbnailMaterialCB));
    if (!tr.lightCB.IsValid())
        tr.lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    if (!tr.shadowCB.IsValid())
        tr.shadowCB = resources.CreateConstantBuffer(sizeof(scene::ShadowConstantsCB));

    return tr.pso.IsValid() && tr.frameCB.IsValid() && tr.objectCB.IsValid() &&
           (!requireFallbackMaterial || tr.materialCB.IsValid()) && tr.lightCB.IsValid() && tr.shadowCB.IsValid();
}

static bool RenderMeshThumbnail(
    renderer::IRenderer& renderer,
    renderer::ResourceManager& resources,
    const renderer::Mesh& mesh,
    renderer::ResourceHandle<renderer::RenderTargetTag> rt,
    renderer::ResourceHandle<renderer::TextureTag> albedoTexture,
    ImVec4 albedoColor,
    renderer::ResourceHandle<renderer::ShaderTag> materialShader = {},
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialCB = {},
    const std::vector<renderer::ResourceHandle<renderer::TextureTag>>* materialTextures = nullptr,
    ThumbnailShaderFlavor flavor = ThumbnailShaderFlavor::Surface,
    const asset::MaterialAsset* materialAsset = nullptr,
    bool clearRT = true,
    math::Vector3 overrideCenter = {},
    float overrideRadius = -1.0f)  // <0 = use mesh bounds
{
    if (!rt.IsValid() || !mesh.vertexBuffer.IsValid() || !mesh.indexBuffer.IsValid())
        return false;
    const bool useMaterialOverride = materialShader.IsValid();
    if (!EnsureThumbnailGpuResources(resources, s_tr, !useMaterialOverride))
        return false;

    const math::Vector3 center = (overrideRadius >= 0.0f) ? overrideCenter : MeshBoundsCenter(mesh);
    const float radius = std::max(0.0001f, (overrideRadius >= 0.0f) ? overrideRadius : MeshBoundsRadius(mesh, center));
    // WHY: 望遠 (FOV 30) + 遠距離の組み合わせはパースがほぼ消えて正射影に近づき、
    //      球が円板のように平坦に見える。FOV を広げてカメラを寄せ、フレーミングを
    //      ほぼ保ったまま遠近感による立体感を出す。
    const float cameraDistance = radius * 2.9f;

    renderer::Camera camera;
    // WHY: Unity の Material Preview に近い、少し上からの 3/4 ビューにする。
    //      真正面よりも球のハイライト・影・輪郭が読み取りやすくなる。
    camera.m_position = {
        center.x - radius * 2.12f,
        center.y + radius * 1.28f,
        center.z - cameraDistance
    };
    camera.m_aspect = 1.0f;
    camera.m_fovY = 38.0f;
    camera.m_near = 0.01f;
    camera.m_far = std::max(10.0f, cameraDistance + radius * 6.0f);
    camera.LookAt(center);

    scene::PerFrameCB frameData{};
    frameData.view = camera.GetViewMatrix();
    frameData.projection = camera.GetProjectionMatrix();
    frameData.viewProjection = camera.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos = camera.m_position;
    frameData.nearZ = camera.m_near;
    frameData.farZ = camera.m_far;
    resources.Update(s_tr.frameCB, &frameData, sizeof(frameData));

    scene::PerObjectCB objectData{};
    objectData.world = math::Matrix4::Identity();
    objectData.worldInvTranspose = math::Matrix4::Identity();
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCBForDraw = s_tr.objectCB;
    if (flavor == ThumbnailShaderFlavor::Terrain) {
        if (!s_tr.terrainObjectCB.IsValid())
            s_tr.terrainObjectCB = resources.CreateConstantBuffer(sizeof(ThumbnailTerrainCB));
        if (!s_tr.terrainObjectCB.IsValid()) return false;
        const ThumbnailTerrainCB terrainData = BuildThumbnailTerrainCB(materialAsset, frameData.viewProjection);
        resources.Update(s_tr.terrainObjectCB, &terrainData, sizeof(terrainData));
        objectCBForDraw = s_tr.terrainObjectCB;
    } else if (flavor == ThumbnailShaderFlavor::Water) {
        if (!s_tr.waterObjectCB.IsValid())
            s_tr.waterObjectCB = resources.CreateConstantBuffer(sizeof(ThumbnailWaterCB));
        if (!s_tr.waterObjectCB.IsValid()) return false;
        const ThumbnailWaterCB waterData = BuildThumbnailWaterCB(materialAsset, frameData.viewProjection);
        resources.Update(s_tr.waterObjectCB, &waterData, sizeof(waterData));
        objectCBForDraw = s_tr.waterObjectCB;
    } else {
        resources.Update(s_tr.objectCB, &objectData, sizeof(objectData));
    }

    if (flavor == ThumbnailShaderFlavor::Skinned) {
        if (!s_tr.skinningCB.IsValid())
            s_tr.skinningCB = resources.CreateConstantBuffer(sizeof(ThumbnailSkinningCB));
        if (!s_tr.skinningCB.IsValid()) return false;
        ThumbnailSkinningCB skinningData{};
        for (auto& bone : skinningData.boneMatrices)
            bone = math::Matrix4::Identity();
        resources.Update(s_tr.skinningCB, &skinningData, sizeof(skinningData));
    }

    if (!useMaterialOverride) {
        ThumbnailMaterialCB materialData{};
        materialData.albedo = {
            std::clamp(albedoColor.x, 0.0f, 1.0f),
            std::clamp(albedoColor.y, 0.0f, 1.0f),
            std::clamp(albedoColor.z, 0.0f, 1.0f),
            std::clamp(albedoColor.w, 0.0f, 1.0f)
        };
        materialData.textureMask = albedoTexture.IsValid() ? 1u : 0u;
        resources.Update(s_tr.materialCB, &materialData, sizeof(materialData));
    }

    // ── 3 点照明リグ (キー / フィル / リム) ──
    // WHY: 単一平行光 + 高いアンビエントでは陰影のグラデーションが浅く、球が円板の
    //      ように平坦に見える。アンビエントを落として明暗差を作り、寒色フィルで陰側の
    //      丸みを読ませ、背後からのリムライトで輪郭を背景から分離して立体感を出す。
    renderer::LightConstantsCB lightData{};
    const math::Vector3 keyLight = (camera.m_position + math::Vector3{ radius * 1.4f, radius * 1.8f, radius * 0.8f } - center).Normalized();
    lightData.lightDir = { -keyLight.x, -keyLight.y, -keyLight.z };
    lightData.lightColor = { 1.0f, 0.96f, 0.90f }; // キー: わずかに暖色
    // 1.8 は「相殺後の最終的な明るさ」。kPreviewUnitScale の定義は下の距離補正コメント参照。
    lightData.lightIntensity = 1.8f / 3.14159265358979323846f;
    lightData.ambientColor = { 0.10f, 0.11f, 0.14f }; // 陰が黒潰れしない下限まで低減

    // WHY: LightAttenuation は 1/dist^2 の絶対距離減衰を含むため、そのままでは
    //      メッシュ半径によってライトの効きが大きく変わる。狙いの明るさになるよう
    //      距離補正を強度へ掛け、どのサイズのプレビューでも同じ見た目にする。
    //      range = radius * 20 に対し dist は radius * 3 前後なので、LightAttenuation の
    //      range 窓 (1-(d/r)^4)^2 はほぼ 1.0 で無視でき、逆二乗だけを打ち消せばよい。
    // NOTE: このリグは「絵として狙った明るさ」を直接指定する手調整値なので、
    //       Lighting.hlsli の LIGHT_UNIT_SCALE (= PI) も相殺する。こうすることで
    //       targetIntensity がそのまま最終的な寄与の強さを表し、シェーダー側の
    //       単位換算を変えてもサムネイルの見た目は動かない。
    constexpr float kPreviewUnitScale = 3.14159265358979323846f; // Lighting.hlsli の LIGHT_UNIT_SCALE
    const auto placeThumbnailLight = [&](renderer::PointLight& light,
                                         const math::Vector3&  offsetFromCenter,
                                         const math::Vector3&  color,
                                         float                 targetIntensity) {
        light.position = center + offsetFromCenter;
        light.color    = color;
        light.range    = radius * 20.0f;
        const float dist = offsetFromCenter.Length();
        // シェーダー側の特異点ガード max(d*d, 0.01) と同じ下限を掛け、
        // 極小メッシュで補正が過剰にならないようにする。
        light.intensity = targetIntensity * std::max(dist * dist, 0.01f) / kPreviewUnitScale;
    };
    // フィル: カメラ側右下から寒色を弱く当て、キーの逆サイドの形状を読ませる
    placeThumbnailLight(lightData.pointLights[0],
                        { radius * 2.6f, -radius * 1.4f, -radius * 2.2f },
                        { 0.55f, 0.65f, 1.0f }, 0.4f);
    // リム: 右上背後からの白。グレージング角のハイライトで輪郭を浮かせる
    placeThumbnailLight(lightData.pointLights[1],
                        { radius * 1.6f, radius * 2.4f, radius * 2.8f },
                        { 1.0f, 1.0f, 1.0f }, 1.1f);
    lightData.pointLightCount = 2;
    resources.Update(s_tr.lightCB, &lightData, sizeof(lightData));

    scene::ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection = math::Matrix4::Translate({ 16.0f, 16.0f, 0.0f });
    shadowData.shadowMapTexelSize[0] = 0.0f;
    shadowData.shadowMapTexelSize[1] = 0.0f;
    // NDC 深度最大値 (1.0) をバイアスにすることで depth - bias <= 0 が常に成立し、
    // SampleCmpLevelZero が必ず 1.0 (照らされている) を返してサムネイル描画でのシャドウを無効化する。
    shadowData.shadowBias = 1.0f;
    resources.Update(s_tr.shadowCB, &shadowData, sizeof(shadowData));

    renderer.SetRenderTarget(rt, resources);
    if (clearRT) {
        renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
        renderer.ClearDepth();
    }
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::CLAMP_LINEAR);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_ANISOTROPIC);

    renderer::DrawCall dc;
    dc.vertexBuffer = mesh.vertexBuffer;
    dc.indexBuffer = mesh.indexBuffer;
    dc.indexCount = mesh.indexCount;
    dc.vertexCount = mesh.vertexCount;
    dc.shader = useMaterialOverride ? materialShader : s_tr.shader;
    dc.pipelineState = s_tr.pso;
    dc.constantBuffers[0] = s_tr.frameCB;
    dc.constantBuffers[1] = objectCBForDraw;
    dc.constantBuffers[2] = (useMaterialOverride && materialCB.IsValid()) ? materialCB : s_tr.materialCB;
    dc.constantBuffers[3] = s_tr.lightCB;
    dc.constantBuffers[4] = s_tr.shadowCB;
    if (flavor == ThumbnailShaderFlavor::Skinned)
        dc.constantBuffers[7] = s_tr.skinningCB;
    if (useMaterialOverride && materialTextures) {
        const size_t count = std::min(dc.textures.size(), materialTextures->size());
        for (size_t i = 0; i < count; ++i)
            dc.textures[i] = (*materialTextures)[i];
        if ((flavor == ThumbnailShaderFlavor::Terrain || flavor == ThumbnailShaderFlavor::Water) &&
            EnsureThumbnailDefaultTextures(resources, s_tr.whiteTexture, s_tr.blackTexture, s_tr.flatNormalTexture)) {
            for (uint32_t i = 0; i < static_cast<uint32_t>(dc.textures.size()); ++i) {
                if (dc.textures[i].IsValid()) continue;
                dc.textures[i] = DefaultThumbnailTextureForSlot(flavor, i, s_tr.whiteTexture, s_tr.blackTexture, s_tr.flatNormalTexture);
            }
        }
    } else {
        dc.textures[0] = albedoTexture;
    }
    renderer.Submit(dc, resources);

    // WHY: AssetBrowser の ImGui 描画中に一時 RT へ切り替えるため、生成後は必ずバックバッファへ戻す。
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    return true;
}

static void DrawThumbnailLabel(ImDrawList* dl, ImVec2 origin, float sz, const char* badge)
{
    const ImVec2 tsz     = ImGui::CalcTextSize(badge);
    const ImVec2 bMin    = { origin.x + sz - tsz.x - 12.0f, origin.y + sz - tsz.y - 7.0f };
    const ImVec2 bMax    = { origin.x + sz - 4.0f,           origin.y + sz - 3.0f         };
    dl->AddRectFilled(bMin, bMax, IM_COL32(20, 22, 26, 205), 3.0f);
    dl->AddText({ bMin.x + 4.0f, bMin.y + 2.0f }, IM_COL32(235, 240, 245, 230), badge);
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

template<typename T>
static void EnsureThumbnailRT(T& t, EditorContext& ctx) {
    if (!t.thumbnailRT.IsValid()) {
        t.thumbnailRT = ctx.resources->CreateRenderTarget(128, 128);
        t.thumbnailRendered = false;
    }
}

// マテリアルサムネイルの GPU 側キャッシュ (シェーダー / CB / テクスチャ) を捨て、
// 次フレームで RebuildMaterialThumbnailGpuData から作り直させる。
// WHY: .mat の再読み込み経路がディスク更新と Inspector 編集の 2 つあり、
//      どちらも同じ後始末を必要とするため 1 箇所にまとめる。
template<typename T>
static void ResetMaterialPreviewGpuState(T& preview, renderer::ResourceManager* resources) {
    preview.previewTexture = {};
    preview.previewTexturePath.clear();
    preview.previewTextureWidth = 0;
    preview.previewTextureHeight = 0;
    preview.shaderPath.clear();
    preview.shader = {};
    if (resources && preview.materialCB.IsValid()) {
        resources->Release(preview.materialCB);
        preview.materialCB = {};
    }
    preview.textures.clear();
    preview.paramData.clear();
    preview.thumbnailRendered = false;
}

// Returns true if the thumbnail was drawn; caller should `return` immediately.
template<typename T>
static bool DrawThumbnailIfReady(T& t, ImVec2 origin, float sz,
                                  EditorContext& ctx, bool hovered, const char* badge) {
    if (!t.thumbnailRendered) return false;
    DrawRenderTargetThumbnail(t.thumbnailRT, origin, sz, ctx, hovered, badge);
    return true;
}

} // namespace

void AssetBrowserPanel::QueueAssetMove(const std::string& sourcePath, const std::string& targetDir)
{
    if (sourcePath.empty() || targetDir.empty() || m_pendingAssetMove.active) return;

    m_pendingAssetMove.sourcePath = sourcePath;
    m_pendingAssetMove.targetDir = targetDir;
    m_pendingAssetMove.active = true;
}

void AssetBrowserPanel::FinalizePendingAssetMove(EditorContext& ctx)
{
    if (!m_pendingAssetMove.active) return;

    // 先にキューを空にする。失敗時も同じ payload が次フレームに残らないようにする。
    const PendingAssetMove request = std::move(m_pendingAssetMove);
    m_pendingAssetMove = {};

    std::string srcAbs;
    std::string dstAbs;
    if (!MoveProjectAssetToDirectory(request.sourcePath, request.targetDir,
                                     ctx, srcAbs, dstAbs)) {
        Toast::Error("Move failed: " + request.sourcePath);
        return;
    }

    if (util::FileSystem::SamePathText(ctx.selectedAssetPath, srcAbs))
        ctx.selectedAssetPath.clear();
    ResetAssetPreviewCache(srcAbs);
    InvalidateTreeCache(util::FileSystem::GetDirectory(srcAbs));
    InvalidateTreeCache(request.targetDir);

    // ここは全アイテムの描画が終わった後なので、m_entries を安全に再構築できる。
    RefreshDirectory();
    ctx.requestAssetBrowserRefresh = false;
    Toast::Success("Moved " + util::FileSystem::GetFilename(dstAbs));
}

ImVec4 AssetBrowserPanel::EntryColor(const Entry& e)
{
    if (e.isDir) return { 0.80f, 0.60f, 0.10f, 1.0f };
    if (const ExtGroup* g = FindGroup(e.ext)) return g->color;
    if (e.ext.empty()) return { 0.38f, 0.38f, 0.38f, 1.0f };
    // 未知拡張子: ハッシュで自動着色
    return ColorFromExt(e.ext);
}

const char* AssetBrowserPanel::EntryLabel(const Entry& e)
{
    if (e.isDir) return "DIR";
    if (const ExtGroup* g = FindGroup(e.ext)) return g->label;
    // 未知拡張子: 拡張子文字列をそのままラベルに使う (最大 6 文字、先頭の . を除く)
    // WHY: 静的バッファに詰めることでどんな拡張子でもラベル表示できる。
    //      ImGui はフレーム内で文字列を参照するため static thread_local を使う。
    static thread_local char buf[8];
    const char* src = e.ext.size() > 1 ? e.ext.c_str() + 1 : e.ext.c_str(); // skip '.'
    const std::string upper = util::StringUtils::ToUpper(src);
    const std::size_t len = std::min<std::size_t>(upper.size(), 6);
    std::memcpy(buf, upper.data(), len);
    buf[len] = '\0';
    return len > 0 ? buf : "FILE";
}

// ─── フォルダツリー (左ペイン) ───────────────────────────────────────────────

void AssetBrowserPanel::DrawFolderTree(const std::string& dirPath, EditorContext& ctx)
{
    const std::string normDir = util::FileSystem::NormalizePathSeparators(dirPath);
    auto it = m_treeCache.find(normDir);
    if (it == m_treeCache.end()) {
        std::vector<Entry> newDirs;
        for (const auto& p : util::FileSystem::ListAll(normDir)) {
            if (!util::FileSystem::IsDirectory(p)) continue;
            Entry e;
            e.path = util::FileSystem::NormalizePathSeparators(p);
            e.name = util::FileSystem::GetFilename(p);
            e.isDir = true;
            if (!ShouldDisplayEntry(e.path, e.name, true)) continue;
            newDirs.push_back(std::move(e));
        }
        // WHY: マウント (外部フォルダ) は Assets ツリーには混ぜず、左ペインの "EXTERNAL"
        //      セクション (OnRenderContent) で専用に列挙する。ここでは実フォルダのみ扱う。
        std::stable_sort(newDirs.begin(), newDirs.end(), [](const Entry& a, const Entry& b) {
            return a.name < b.name;
        });
        it = m_treeCache.emplace(normDir, std::move(newDirs)).first;
    }
    // WHY: 参照ではなくコピーを取る。
    //      再帰 DrawFolderTree / RefreshDirectory() が m_treeCache に insert/erase すると
    //      unordered_map のリハッシュや対象エントリ削除で参照が無効化 (UB) されクラッシュする。
    const std::vector<Entry> dirs = it->second;

    for (const Entry& dir : dirs) {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_SpanAvailWidth;
        const bool isCurrent = util::FileSystem::SamePathText(dir.path, m_currentPath);
        if (isCurrent) flags |= ImGuiTreeNodeFlags_Selected;

        // WHY: 表示名は Assets 側の仮想名、ID は実パスにすることで同名マウントでも ImGui ID が衝突しない。
        // 現在フォルダはアクセント色の塗りで強調する (既定の薄い選択色より目立たせる)。
        if (isCurrent)
            ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        bool open = ImGui::TreeNodeEx(dir.path.c_str(), flags, "%s", dir.name.c_str());
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
                    // Undo 履歴には積まない。旧実装の Undo は RemoveAll(newDir) で、
                    // 作成後にユーザーがそこへ入れたアセットまで巻き添えで消していた。
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
                    // 複製も Undo 対象外 (削除と同じく、消したいときは Delete から
                    // ごみ箱へ送る)。Ctrl+Z でフォルダごと RemoveAll されない。
                    m_currentPath = normDir;
                    InvalidateTreeCache(normDir);
                    RefreshDirectory();
                    BeginRenameForPath(dstPath, &ctx);
                }
            }
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
        // ヒエラルキーエンティティをフォルダノードにドロップ → そのフォルダへ Prefab 保存
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

// ─── アイコン描画ユーティリティ ──────────────────────────────────────────────

void AssetBrowserPanel::DrawFileIconAt(ImVec2 origin, float sz, const Entry& e, bool hovered)
{
    const ImVec4 base  = EntryColor(e);
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

// ─── アイコン1個 (右ペイン) ──────────────────────────────────────────────────

void AssetBrowserPanel::DrawAssetPreviewIconAt(ImVec2 origin, float sz, const Entry& e, EditorContext& ctx, bool hovered)
{
    if (e.isDir) {
        DrawFileIconAt(origin, sz, e, hovered);
        return;
    }

    if ((IsTextureExt(e.ext) || e.isSpriteSubAsset) && ctx.resources && ctx.imguiRenderer) {
        const std::string& texturePath = e.isSpriteSubAsset ? e.sourceAssetPath : e.path;
        TexturePreview& preview = m_texturePreviews[texturePath];
        if (!preview.handle.IsValid() && !preview.failed && !preview.queued) {
            preview.queued = true;
            m_texLoadQueue.push_back(texturePath);
        }

        if (!preview.failed) {
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

    // 画像 + .meta sidecar: ImageImporter 経由でロードして GPU テクスチャを表示
    if (e.ext == ".tex" && ctx.resources && ctx.imguiRenderer) {
        TexDescPreview& preview = m_texDescPreviews[e.path];
        const auto currentWriteTime = ReadLastWriteTime(e.path);
        if (currentWriteTime != preview.lastWriteTime) {
            preview.lastWriteTime = currentWriteTime;
            preview.handle = {};
            preview.width = preview.height = 0;
            preview.failed = false;
        }
        if (!preview.handle.IsValid() && !preview.failed) {
            preview.handle = asset::AssetManager::Load<asset::TextureAsset>(e.path);
            if (preview.handle.IsValid()) {
                if (const auto* ta = asset::AssetManager::Get(preview.handle)) {
                    if (const auto* tex = ctx.resources->Get(ta->gpuHandle)) {
                        preview.width  = tex->GetWidth();
                        preview.height = tex->GetHeight();
                    }
                }
            } else {
                preview.failed = true;
            }
        }
        if (!preview.failed && preview.handle.IsValid()) {
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
        if (!preview.loaded || currentWriteTime != preview.lastWriteTime) {
            preview.asset = {};
            preview.failed = !asset::LoadMaterialAssetFromFile(e.path, preview.asset);
            preview.loaded = true;
            preview.lastWriteTime = currentWriteTime;
            ResetMaterialPreviewGpuState(preview, m_resources);
        }

        // Inspector で編集中の .mat は、保存を待たずにサムネイルへ反映する。
        // WHY: ファイル更新時刻だけを見ていると、スライダーを動かしている最中の見た目が
        //      AssetBrowser 側だけ古いままになる。Inspector が値変更のたびに進める
        //      リビジョンを検知して、AssetManager 上の (未保存の) 実体からサムネイルを描き直す。
        if (!ctx.materialPreviewRevisions.empty()) {
            const std::string relPath = NormalizeAssetPath(e.path);
            const uint64_t revision = ctx.MaterialPreviewRevision(relPath);
            if (revision != 0 && revision != preview.liveRevision) {
                preview.liveRevision = revision;
                if (const auto* live = asset::AssetManager::GetMaterial(
                        asset::AssetManager::LoadMaterial(relPath))) {
                    preview.asset  = *live;
                    preview.failed = false;
                    preview.loaded = true;
                    // ここでは GPU リソースを捨てない。RebuildMaterialThumbnailGpuData が
                    // シェーダー変更を検知して張り替え、テクスチャと定数バッファは毎回更新するため、
                    // 再描画フラグを落とすだけで足りる。
                    // WHY: スライダーをドラッグしている間は毎フレームここを通るので、
                    //      定数バッファを作り直すとハンドルの生成/破棄が延々と続いてしまう。
                    preview.thumbnailRendered = false;
                }
            }
        }

        // シェーダーファイルが変更された場合もサムネイルをリセットする
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

            EnsureThumbnailRT(preview, ctx);
            if (!preview.thumbnailRendered && preview.thumbnailRT.IsValid()) {
                if (!s_tr.materialSphere)
                    s_tr.materialSphere = renderer::PrimitiveMesh::Sphere(*ctx.resources, 64);
                if (!s_tr.skinnedMaterialSphere)
                    s_tr.skinnedMaterialSphere = CreateSkinnedPreviewSphere(*ctx.resources, 64);
                if (!s_tr.waterMaterialSphere)
                    s_tr.waterMaterialSphere = CreateWaterPreviewSphere(*ctx.resources, 64);
                const ThumbnailShaderFlavor flavor = DetectMaterialThumbnailFlavor(preview.asset);
                renderer::Mesh* previewMesh = nullptr;
                if (flavor == ThumbnailShaderFlavor::Skinned)
                    previewMesh = s_tr.skinnedMaterialSphere;
                else if (flavor == ThumbnailShaderFlavor::Water)
                    previewMesh = s_tr.waterMaterialSphere;
                else if (flavor == ThumbnailShaderFlavor::Surface || flavor == ThumbnailShaderFlavor::Terrain)
                    previewMesh = s_tr.materialSphere;

                if (flavor == ThumbnailShaderFlavor::Unsupported && preview.previewTexture.IsValid()) {
                    if (void* rawID = ctx.imguiRenderer->GetImTextureID(preview.previewTexture, *ctx.resources)) {
                        DrawTextureThumbnail(rawID, preview.previewTextureWidth, preview.previewTextureHeight, origin, sz, hovered);
                        return;
                    }
                }

                if (previewMesh && RebuildMaterialThumbnailGpuData(preview, ctx)) {
                    preview.thumbnailRendered = RenderMeshThumbnail(
                        *ctx.renderer,
                        *ctx.resources,
                        *previewMesh,
                        preview.thumbnailRT,
                        preview.previewTexture,
                        SelectMaterialColor(preview.asset),
                        preview.shader,
                        preview.materialCB,
                        &preview.textures,
                        flavor,
                        &preview.asset);
                }
            }
            if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "MAT")) return;
        }
    }

    // インポート済み .fbx: LOD0 サブメッシュ + パッケージマテリアルを 3D プレビュー
    // WHY: FBX を第一級アセットとして扱い、内部 .fzasset コンテナを UI へ露出しないため。
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
        // 物理実体は Library/Baked の内部コンテナなので、更新検知は解決後のパスで行う。
        const auto currentWriteTime = ReadLastWriteTime(previewWritePath);
        if (currentWriteTime != preview.lastWriteTime) {
            if (m_resources)
                for (auto& mp : preview.slotMaterials)
                    if (mp.materialCB.IsValid())
                        m_resources->Release(mp.materialCB);
            preview.slotMaterials.clear();
            preview.materialsLoaded = false;
            preview.lastWriteTime = currentWriteTime;
            preview.handle = {};
            preview.thumbnailRendered = false;
            preview.failed = false;
        }
        EnsureThumbnailRT(preview, ctx);
        if (!preview.handle.IsValid() && !preview.failed) {
            FBZZ_LOG_INFO("AssetBrowserItems: Load<ModelAsset> [%s]", previewModelPath.c_str());
            // WHY: インポート直後やファイル監視直後は、生成前に一度 Load して Null が
            //      AssetManager にキャッシュされることがある。サムネイル再試行時は失敗 cache を掃除する。
            asset::AssetManager::FlushFailed();
            preview.handle = asset::AssetManager::Load<asset::ModelAsset>(previewModelPath);
            if (!preview.handle.IsValid()) {
                FBZZ_LOG_ERROR("AssetBrowserItems: ModelAsset load failed [%s]", previewModelPath.c_str());
                preview.failed = true;
            }
        }
        // マテリアルスロットを初回ロード (materials/slotName.mat -> per-slot MaterialPreview)
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
        if (!preview.thumbnailRendered && !preview.failed && preview.thumbnailRT.IsValid()) {
            const asset::ModelAsset* m = asset::AssetManager::Get(preview.handle);
            if (m && !m->lods.empty() && !m->lods[0].submeshes.empty()) {
                // 全サブメッシュの AABB から共通カメラを計算 (Unity 同様すべてのメッシュが写る)
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
                    // WHY: resources 未初期化時にロードされた場合 GPU バッファが未作成。CPU データから lazily 作成。
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
                        if (DetectMaterialThumbnailFlavor(matPrev->asset) == ThumbnailShaderFlavor::Unsupported ||
                            !RebuildMaterialThumbnailGpuData(*matPrev, ctx)) {
                            matPrev = nullptr;
                        }
                    }
                    const bool ok = RenderMeshThumbnail(
                        *ctx.renderer, *ctx.resources,
                        *mesh, preview.thumbnailRT,
                        renderer::ResourceHandle<renderer::TextureTag>{},
                        matPrev ? SelectMaterialColor(matPrev->asset) : ImVec4{ 0.74f, 0.78f, 0.84f, 1.0f },
                        matPrev ? matPrev->shader : renderer::ResourceHandle<renderer::ShaderTag>{},
                        matPrev ? matPrev->materialCB : renderer::ResourceHandle<renderer::ConstantBufferTag>{},
                        matPrev ? &matPrev->textures : nullptr,
                        matPrev ? DetectMaterialThumbnailFlavor(matPrev->asset) : ThumbnailShaderFlavor::Surface,
                        matPrev ? &matPrev->asset : nullptr,
                        firstDraw, combinedCenter, combinedRadius);
                    if (ok) { preview.thumbnailRendered = true; firstDraw = false; }
                }
            }
            if (!preview.thumbnailRendered) preview.failed = true;
        }
        if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "FBX")) return;
    }

    // 仮想 .mesh サブアセット (::mesh:: 合成パス): .fzasset 内の特定サブメッシュを 3D プレビュー
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
                preview.failed           = false;
            }
            EnsureThumbnailRT(preview, ctx);
            if (!preview.handle.IsValid() && !preview.failed) {
                asset::AssetManager::FlushFailed();
                preview.handle = asset::AssetManager::Load<asset::ModelAsset>(parentPath);
                if (!preview.handle.IsValid()) preview.failed = true;
            }
            if (!preview.thumbnailRendered && !preview.failed && preview.thumbnailRT.IsValid()) {
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
                if (!preview.thumbnailRendered) preview.failed = true;
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
            preview.failed = false;
        }
        EnsureThumbnailRT(preview, ctx);
        if (!preview.model && !preview.failed)
            preview.model = asset::AssetManager::LoadModel(e.path);
        if (!preview.thumbnailRendered && !preview.failed && preview.thumbnailRT.IsValid() &&
            preview.model && !preview.model->meshes.empty() && preview.model->meshes.front()) {
            preview.thumbnailRendered = RenderMeshThumbnail(
                *ctx.renderer,
                *ctx.resources,
                *preview.model->meshes.front(),
                preview.thumbnailRT,
                renderer::ResourceHandle<renderer::TextureTag>{},
                { 0.74f, 0.78f, 0.84f, 1.0f });
            preview.failed = !preview.thumbnailRendered;
        }
        const char* badge = (e.ext == ".asset") ? "ASSET" : "MESH";
        if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, badge)) return;
    }

    // .prefab: TOML を解析してメッシュを持つ場合は 3D サムネイル、なければキューブアイコン
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
                                // SkinnedMeshRenderer を優先 (フルモデルパス)
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
            if (preview.hasMesh && !preview.failed) {
                EnsureThumbnailRT(preview, ctx);
                if (!preview.model) {
                    std::string absPath = preview.meshPath;
                    if (absPath.starts_with("Assets/") && !ctx.projectRoot.empty())
                        absPath = ctx.projectRoot + "/" + absPath;
                    preview.model = asset::AssetManager::LoadModel(absPath);
                }
                if (!preview.thumbnailRendered && preview.thumbnailRT.IsValid() &&
                    preview.model && !preview.model->meshes.empty() && preview.model->meshes.front()) {
                    preview.thumbnailRendered = RenderMeshThumbnail(
                        *ctx.renderer, *ctx.resources,
                        *preview.model->meshes.front(),
                        preview.thumbnailRT,
                        renderer::ResourceHandle<renderer::TextureTag>{},
                        { 0.35f, 0.82f, 0.95f, 1.0f });
                    preview.failed = !preview.thumbnailRendered;
                }
                if (DrawThumbnailIfReady(preview, origin, sz, ctx, hovered, "PREFAB")) return;
            }
        }
        // フォールバック: アイソメトリックキューブアイコン
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
            const float hw = sz * 0.22f; // front face half-width
            const float hh = sz * 0.20f; // front face half-height
            const float dx = sz * 0.14f; // depth x-offset
            const float dy = sz * 0.09f; // depth y-offset

            // front face
            ImVec2 frontFace[4] = {
                { cx - hw,      cy - hh },
                { cx + hw,      cy - hh },
                { cx + hw,      cy + hh },
                { cx - hw,      cy + hh },
            };
            dl->AddConvexPolyFilled(frontFace, 4, front);

            // top face
            ImVec2 topFace[4] = {
                { cx - hw,      cy - hh      },
                { cx + hw,      cy - hh      },
                { cx + hw + dx, cy - hh - dy },
                { cx - hw + dx, cy - hh - dy },
            };
            dl->AddConvexPolyFilled(topFace, 4, top);

            // right face
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

    // .animcontroller: ステートマシン風アイコン (3ノード + 矢印)
    if (e.ext == ".animcontroller") {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg   = IM_COL32( 50,  90,  65, 255);
        const ImU32 node = IM_COL32( 80, 200, 120, 255);
        const ImU32 edge = IM_COL32(200, 255, 200, 180);
        const ImU32 brd  = IM_COL32(160, 220, 160, 255);
        dl->AddRectFilled({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, bg, sz * 0.08f);
        dl->AddRect      ({ origin.x, origin.y }, { origin.x + sz, origin.y + sz }, brd, sz * 0.08f, 0, 1.0f);
        // node positions: left-mid, top-right, bottom-right
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

    // .terrain: layerMaterials[0] を読み取って layer0 diffuse でサムネイル、なければ丘アイコン
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
                        if (const auto* layerArr = (*terrainTbl)["layerMaterials"].as_array();
                            layerArr && !layerArr->empty())
                            matPath = (*layerArr)[0].value_or(std::string{});
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
            if (preview.hasMaterial && !preview.mat.failed) {
                EnsureThumbnailRT(preview.mat, ctx);
                if (!preview.mat.thumbnailRendered) {
                    if (!s_tr.materialSphere)
                        s_tr.materialSphere = renderer::PrimitiveMesh::Sphere(*ctx.resources, 64);
                    if (s_tr.materialSphere && RebuildMaterialThumbnailGpuData(preview.mat, ctx)) {
                        preview.mat.thumbnailRendered = RenderMeshThumbnail(
                            *ctx.renderer, *ctx.resources,
                            *s_tr.materialSphere,
                            preview.mat.thumbnailRT,
                            preview.mat.previewTexture,
                            SelectMaterialColor(preview.mat.asset),
                            preview.mat.shader,
                            preview.mat.materialCB,
                            &preview.mat.textures,
                            ThumbnailShaderFlavor::Terrain,
                            &preview.mat.asset);
                        preview.mat.failed = !preview.mat.thumbnailRendered;
                    }
                }
                if (DrawThumbnailIfReady(preview.mat, origin, sz, ctx, hovered, "TERRAIN")) return;
            }
        }
        // フォールバック: 丘シルエットアイコン
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
        if (preview.handle.IsValid() || preview.failed) continue;
        preview.handle = ctx.resources->LoadTexture(ToTextureLoadPath(path, ctx));
        if (preview.handle.IsValid()) {
            if (auto* texture = ctx.resources->Get(preview.handle)) {
                preview.width  = texture->GetWidth();
                preview.height = texture->GetHeight();
            }
        } else {
            preview.failed = true;
        }
    }
}

void AssetBrowserPanel::ResetAssetPreviewCache(const std::string& path)
{
    m_texturePreviews.erase(path);
    m_spritePreviews.erase(path);
    auto releaseAndErase = [&](auto& map) {
        auto it = map.find(path);
        if (it == map.end()) return;
        if (m_resources) {
            if constexpr (requires { it->second.thumbnailRT; }) {
                if (it->second.thumbnailRT.IsValid())
                    m_resources->Release(it->second.thumbnailRT);
                if constexpr (requires { it->second.materialCB; }) {
                    if (it->second.materialCB.IsValid())
                        m_resources->Release(it->second.materialCB);
                }
            } else if constexpr (requires { it->second.mat.thumbnailRT; }) {
                if (it->second.mat.thumbnailRT.IsValid())
                    m_resources->Release(it->second.mat.thumbnailRT);
                if (it->second.mat.materialCB.IsValid())
                    m_resources->Release(it->second.mat.materialCB);
            }
        }
        map.erase(it);
    };
    releaseAndErase(m_materialPreviews);
    releaseAndErase(m_meshPreviews);
    releaseAndErase(m_prefabPreviews);
    releaseAndErase(m_terrainPreviews);
    {
        auto it = m_modelAssetPreviews.find(path);
        if (it != m_modelAssetPreviews.end() && m_resources)
            for (auto& mp : it->second.slotMaterials)
                if (mp.materialCB.IsValid())
                    m_resources->Release(mp.materialCB);
    }
    releaseAndErase(m_modelAssetPreviews);
    // 合成パス (path::mesh::N) で登録されたサブメッシュプレビューもクリア
    {
        const std::string synthPrefix = path + "::mesh::";
        for (auto it = m_modelAssetPreviews.begin(); it != m_modelAssetPreviews.end(); ) {
            if (it->first.starts_with(synthPrefix)) {
                if (m_resources) {
                    if (it->second.thumbnailRT.IsValid())
                        m_resources->Release(it->second.thumbnailRT);
                    for (auto& mp : it->second.slotMaterials)
                        if (mp.materialCB.IsValid())
                            m_resources->Release(mp.materialCB);
                }
                it = m_modelAssetPreviews.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_texDescPreviews.erase(path);
}

// ── DrawEntry サブメソッド ──────────────────────────────────────────────────────

void AssetBrowserPanel::DrawEntryBadges(ImDrawList* dl, ImVec2 origin, float sz, const Entry& e)
{
    // 選択・ホバー表現は DrawEntry 側 (SelectionVisuals) が担当する。ここは状態バッジのみ。
    // ! バッジ: 未変換モデルファイルに赤丸で警告表示
    // WHY: FBX は正規モデルアセットとして扱うため、未変換警告は出さない。
    if (!e.isDir && IsImportableRaw(e.ext) && e.ext != ".fbx") {
        const float r  = sz * 0.15f;
        const float cx = origin.x + sz - r;
        const float cy = origin.y + r;
        dl->AddCircleFilled({ cx, cy }, r, IM_COL32(220, 50, 50, 230));
        const ImVec2 bsz = ImGui::CalcTextSize("!");
        dl->AddText({ cx - bsz.x * 0.5f, cy - bsz.y * 0.5f }, IM_COL32(255, 255, 255, 255), "!");
    }
    // ↻ バッジ (再インポートが必要) はここにあった。
    // WHY 消したか: 原本や import 設定の変更はウォッチャーが拾って自動で焼き直すので、
    //     「古い」状態は人が見て対処する対象ではなくなった。焼き直しの最中は
    //     EditorTaskOverlay が出るため、そこで進行は分かる。
    // 橙ドット: 未保存変更があるアセット
    if (!e.isDir && AssetDirtyRegistry::IsDirty(e.path)) {
        const float r  = sz * 0.10f;
        const float cx = origin.x + r + 2.0f;
        const float cy = origin.y + r + 2.0f;
        dl->AddCircleFilled({ cx, cy }, r, IM_COL32(255, 160, 30, 230));
    }
    // ▶/▼ 展開トグル: FBX と Sprite Texture はサブアセットを持つ。
    // WHY: 素の三角形はサムネイルの絵柄に溶けて「押せる場所」に見えなかった。
    //      暗いチップ (角丸の下地) に乗せてボタンらしさを与え、展開中は
    //      アクセント色にしてサブアセットの帯と対応付ける。
    if (!e.isDir && !e.isSubAsset && e.hasSubAssets) {
        const bool  expanded = m_expandedAssets.count(e.path) > 0;
        const float ts   = sz * 0.18f;                 // 三角サイズ (クリック判定と共通)
        const float bx   = origin.x + 2.0f;
        const float by   = origin.y + sz - ts - 2.0f;
        const float pad  = ts * 0.35f;
        dl->AddRectFilled({ bx - pad, by - pad }, { bx + ts + pad, by + ts + pad },
                          IM_COL32(18, 20, 24, 190), 4.0f);
        const ImU32 col = expanded
            ? EditorTheme::ColorU32(ThemeColor::Accent, 1.0f)
            : IM_COL32(226, 232, 240, 235);
        if (expanded) {
            // ▼ (pointing down)
            dl->AddTriangleFilled(
                { bx,        by },
                { bx + ts,   by },
                { bx + ts * 0.5f, by + ts },
                col);
        } else {
            // ▶ (pointing right)
            dl->AddTriangleFilled(
                { bx,        by },
                { bx,        by + ts },
                { bx + ts,   by + ts * 0.5f },
                col);
        }
    }
    // サブアセットの帯 (親と子を繋ぐ面) は DrawEntry 側の DrawSubAssetBand が描く。
}

void AssetBrowserPanel::HandleEntryClick(const Entry& e, EditorContext& ctx, bool hov)
{
    // マウス押下フレーム: ドラッグ・ダブルクリックフラグをリセット (選択はまだしない)
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_entryDragStarted    = false;
        m_doubleClickConsumed = false;
    }

    // 選択確定はマウスリリース時 (Unity スタイル: D&D 開始後はスキップ)
    if (!hov || !ImGui::IsMouseReleased(ImGuiMouseButton_Left) || e.isDir) return;
    if (m_entryDragStarted) return;
    if (m_doubleClickConsumed) { m_doubleClickConsumed = false; return; }

    const bool ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);

    if (ctrl) {
        if (m_selectedPaths.count(e.path)) m_selectedPaths.erase(e.path);
        else                                m_selectedPaths.insert(e.path);
        ctx.selectedAssetPath = e.path;
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
        ctx.selectedAssetPath = e.path;
        m_pendingRenamePath.clear();
    } else {
        // 選択済み & 単体選択状態での再クリック → 遅延リネーム (Unity スタイル)
        // 合成パス (::mesh:: 仮想サブアセット) はリネーム不可
        const bool canRename = !e.isMount && !e.isPackageAsset && !e.isSubAsset;
        if (canRename && ctx.selectedAssetPath == e.path && m_selectedPaths.empty()) {
            m_pendingRenamePath  = e.path;
            m_pendingRenameTimer = static_cast<float>(ImGui::GetTime());
        } else {
            m_selectedPaths.clear();
            ctx.selectedAssetPath = e.path;
            m_lastClickedPath     = e.path;
            m_pendingRenamePath.clear();
            // FBX コンテンツ更新をクリック時に実施 (ホバーから移行)
            if (IsMeshExt(e.ext)) {
                m_selectedFbxPath = e.path;
                m_selectedModel   = nullptr;
                if (renderer::ResourceManager::Active())
                    m_selectedModel = asset::AssetManager::LoadModel(e.path);
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
    m_pendingRenamePath.clear();   // ダブルクリックは遅延リネームをキャンセル
    m_doubleClickConsumed = true;  // 2回目リリースで HandleEntryClick をスキップ

    // WHY: ダブルクリック後に entries が更新される可能性があるため値をコピーする。
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
            ctx.selectedEntities.clear();
            if (ctx.undoStack) ctx.undoStack->Clear();
            if (ctx.markSceneDirty) ctx.markSceneDirty();
            FBZZ_LOG_INFO("Opened scene: %s", path.c_str());
        } else {
            FBZZ_LOG_ERROR("Failed to open scene: %s", path.c_str());
        }
    } else if (ext == ".prefab" && ctx.activeScene && ImGui::GetIO().KeyAlt) {
        // Alt+ダブルクリック: シーンへ置くのではなく、プレファブ本体を編集面で開く。
        // WHY: 既定はこれまでどおり「配置」。編集は破壊的になりうるので、
        //      明示的な修飾キーと右クリックメニューからだけ入れるようにする。
        ctx.requestOpenPrefabEdit = NormalizeAssetPath(path);
    } else if (ext == ".prefab" && ctx.activeScene) {
        const bool canRecordUndo =
            ctx.undoStack != nullptr && ctx.undoStack->IsRecordingEnabled();
        const std::string before = canRecordUndo
            ? SceneIO::Serialize(*ctx.activeScene)
            : std::string{};
        std::vector<scene::EntityID> roots;
        if (PrefabSerializer::Instantiate(*ctx.activeScene, path, roots)) {
            ctx.selectedEntities = roots;
            const std::string after = canRecordUndo
                ? SceneIO::Serialize(*ctx.activeScene)
                : std::string{};
            if (canRecordUndo && before != after) {
                scene::Scene* scene = ctx.activeScene;
                EditorContext* context = &ctx;
                const auto markDirty = ctx.markSceneDirty;
                auto restore = [scene, context, markDirty](const std::string& snapshot) {
                    if (SceneIO::Deserialize(*scene, snapshot)) {
                        context->selectedEntities.clear();
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
    } else if (ext == ".animcontroller" || ext == ".vfx" || ext == ".behaviortree") {
        // ドキュメント面へ渡す振り分けは asset.open operator が持つ。
        // WHY 写さないか: 同じ分岐がコマンドパレットと SearchEverything にもあり、
        //      そちらは .behaviortree を落としていた (このパネルからしか開けなかった)。
        //      対応拡張子を足したときに全経路へ同時に効く形にしておく。
        OpArgs args;
        args.Set("path", path);
        InvokeOperator(ctx, "asset.open", args);
    }
}

// 選択中パスのスナップショットをクリップボードに積む。
// WHY: OS クリップボードではなく panel ローカルに持つのは、ファイル実体コピーは
//      アプリ終了後に有効である必要がなく (プロセス跨ぎの貼り付けは対象外)、
//      パス文字列コピー (Copy Path) と役割を混同させないため。
void AssetBrowserPanel::CopySelectionToClipboard()
{
    m_clipboardPaths.clear();
    if (!m_selectedPaths.empty()) {
        m_clipboardPaths.assign(m_selectedPaths.begin(), m_selectedPaths.end());
    } else if (!m_lastClickedPath.empty() && util::FileSystem::Exists(m_lastClickedPath)) {
        m_clipboardPaths.push_back(m_lastClickedPath);
    }
}

// 現在開いているフォルダへクリップボードの内容を複製する。
// 生成した実体は Undo 対象外 (取り消したいときは Delete でごみ箱へ送る)。
//
// WHY Library の生成物を「取り出し」として扱うか:
//   Library/Baked の .anim / .mat / textures をコピペすると、.meta を複製しない仕様の
//   おかげで結果的に「新しい GUID を持つ独立アセット」ができる。これは Extract と
//   まったく同じ結果だが、以前は何の説明も出ないため「ただのコピー」に見えていた。
//   同じ結果を出す道が 2 本あって片方だけ意味が語られている状態を解消し、
//   コピペを取り出しの正式な動線として認める。
void AssetBrowserPanel::PasteClipboardAssets(EditorContext& ctx)
{
    if (m_clipboardPaths.empty()) return;
    const std::string destDir = m_currentPath;

    std::vector<std::string> pastedPaths;
    int extractedCount = 0;
    for (const auto& srcPath : m_clipboardPaths) {
        if (!util::FileSystem::Exists(srcPath)) continue;  // 元がリネーム/削除済みなら黙ってスキップ
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

    // 貼り付けた項目をそのまま選択状態にする (Unity と同じく直後にリネーム/移動しやすくする)。
    m_selectedPaths.clear();
    if (pastedPaths.size() == 1) {
        ctx.selectedAssetPath = pastedPaths.front();
        m_lastClickedPath     = pastedPaths.front();
    } else if (pastedPaths.size() > 1) {
        m_selectedPaths.insert(pastedPaths.begin(), pastedPaths.end());
        ctx.selectedAssetPath = pastedPaths.front();
    }

    // 取り出しが起きたことは必ず伝える。黙って独立アセットが増えると、
    // 「なぜ再インポートしても更新されないのか」が後から分からなくなる。
    if (extractedCount > 0) {
        Toast::Success(std::to_string(extractedCount) +
                       " generated asset(s) extracted to Assets");
    }
}

void AssetBrowserPanel::HandleClipboardShortcuts(EditorContext& ctx)
{
    if (!ctx.assetBrowserFocused) return;
    // リネーム中や検索ボックス入力中の Ctrl+C/V はテキスト編集として扱う (横取りしない)。
    if (ImGui::GetIO().WantTextInput) return;

    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
        CopySelectionToClipboard();
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
        PasteClipboardAssets(ctx);
}

bool AssetBrowserPanel::IsBakedLibraryPath(const std::string& absPath)
{
    // 取り出し対象かどうかは拡張子ではなく「どこに居るか」で決まる。
    // Library/Baked = 再インポートで作り直される生成物、Assets = 人の著作物。
    const std::string normalized = util::StringUtils::ToLower(
        util::FileSystem::NormalizePathSeparators(absPath));
    return normalized.find("/library/baked/") != std::string::npos;
}

bool AssetBrowserPanel::IsExtractableSubAsset(const Entry& e)
{
    if (e.isDir || !e.isSubAsset) return false;
    // 仮想サブアセット (Sprite の "path::id" / "::mesh::N") は実ファイルではない。
    if (e.isSpriteSubAsset) return false;
    if (e.path.find("::mesh::") != std::string::npos) return false;
    if (!util::FileSystem::Exists(e.path)) return false;

    // Assets に居る .mat / textures は既に独立した実体なので、複製したいなら
    // 通常の Duplicate を使えばよく、Extract という別概念を増やす必要が無い。
    return IsBakedLibraryPath(e.path);
}

std::string AssetBrowserPanel::ExtractSubAsset(const Entry& e, EditorContext& ctx) const
{
    if (!IsExtractableSubAsset(e)) return {};

    // 取り出し先は原本 FBX の隣。
    // WHY 現在のフォルダではなく原本の隣か: 取り出したクリップは、どのモデルから来たのかが
    //     分からなくなると使い道が消える。原本と同じ場所に置けば対応が保たれる。
    std::string destDir = m_currentPath;
    if (!e.sourceAssetPath.empty())
        destDir = util::FileSystem::GetDirectory(e.sourceAssetPath);
    if (destDir.empty()) destDir = ctx.projectRoot + "/Assets";
    // GetDirectory は末尾に '/' を付けて返す。連結で "//" にならないよう落とす。
    while (!destDir.empty() && (destDir.back() == '/' || destDir.back() == '\\'))
        destDir.pop_back();
    if (!util::FileSystem::EnsureDirectory(destDir)) return {};

    const std::string fileName = util::FileSystem::GetFilename(e.path);
    std::string destPath = destDir + "/" + fileName;
    // 既存を黙って上書きしない。2 回目の Extract は別名で残す。
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

    // .meta は複製しない。
    // WHY: GUID をコピーすると 2 つの実体が同じ GUID を名乗り、参照解決が
    //      どちらを返すか不定になる。.meta を作らずに置けば、AssetDatabase の
    //      スキャンが新しい GUID を採番して独立したアセットになる。
    return destPath;
}

void AssetBrowserPanel::DrawEntryContextMenu(const Entry& e, EditorContext& ctx)
{
    if (!ImGui::BeginPopupContextItem("##entry_ctx")) return;

    // 複数選択時の一括操作
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

    // Sprite サブアセットは実ファイルではないため、複製・削除・リネームを出さず、
    // 元画像を編集する操作と安定参照のコピーだけを提供する。
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

    // Library/Baked に隔離された生成物 (.anim 等) を Assets へ取り出す。
    //
    // WHY 取り出しを用意するか (Unity の "Extract From Prefab" 相当):
    //   隔離した .anim は再インポートのたびに上書きされる。1 本だけ手で調整したい
    //   (イベントを足す・別のクリップとして派生させる) 場合、上書きされない実体が要る。
    //   コピーして Assets へ置き、新しい GUID を振れば独立アセットになり、
    //   以降は原本 FBX の再インポートから切り離される。
    //
    // WHY 元の参照を書き換えないか:
    //   既存のシーンや .animcontroller は Library 側を guid で指している。取り出した
    //   瞬間に全部を新しい方へ向けると、「複製したつもりが元も変わった」ことになる。
    //   取り出した実体を使うかどうかは、人が参照を差し替えて決める。
    if (!e.isDir && e.isSubAsset && !e.isSpriteSubAsset && IsExtractableSubAsset(e)) {
        if (ImGui::MenuItem("Extract to Assets")) {
            const std::string extracted = ExtractSubAsset(e, ctx);
            if (extracted.empty()) {
                Toast::Error("Extract failed: " + e.name);
            } else {
                Toast::Success("Extracted " + util::FileSystem::GetFilename(extracted));
                // 取り出した実体を選択状態にする。
                // WHY: Ctrl+V は貼った項目を選択する。取り出しは「出して続けて編集する」
                //      動線なので、同じ結果になる操作で選択の扱いが違うと迷う。
                m_pendingNavigate = util::FileSystem::GetDirectory(extracted);
                m_selectedPaths.clear();
                m_lastClickedPath     = extracted;
                ctx.selectedAssetPath = extracted;
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

    // プレファブ本体の編集面へ入る動線。
    // WHY: 既定のダブルクリックは「シーンへ配置」なので、アセットそのものを直したい
    //      ときの入口が無かった。シーンに 1 個も置いていないプレファブも編集できる。
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
        // 原本と設定の変更はウォッチャーが自動で焼き直すので、ここは
        // 「変更が無いのに作り直したい」ときの手動経路として残す。
        // ラベルの出し分けは m_outdatedPaths ではなく「既に入っているか」で決める。
        // WHY: 自動化した今、古い印はキュー投入から完了までの一瞬しか立たない。
        //      それをラベルの根拠にすると、ほぼ常に Re-import が Import に見える。
        const bool imported = IsAlreadyImported(e.path);
        if (ImGui::MenuItem(imported ? "\xe2\x86\xbb Re-import" : "Import")) {
            // 自動経路と同じ「処理中」の印で二重投入を防ぐ (完了時に取り除かれる)。
            if (m_outdatedPaths.insert(e.path).second) {
                // 保存済み設定を読み直してから積む。既定の options で押し流すと、
                // 選択メッシュ・クリップ範囲・Loop Time が黙って初期値へ戻る。
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
        // 右クリックした e が事前に左クリック選択されているとは限らないため、
        // m_lastClickedPath 頼みの CopySelectionToClipboard() ではなくこの項目自体を積む。
        const bool copiesGenerated = IsBakedLibraryPath(e.path);
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
            m_clipboardPaths = { e.path };
        // 生成物を掴んだときは、貼り付けが「取り出し」になることを先に言う。
        // WHY: 結果として独立アセットが増えるのに、操作名が Copy のままだと
        //      「再インポートしても更新されない実体」を作った自覚が持てない。
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

        // 探すのは (1) GUID 参照、(2) パス参照 の 2 通り。
        //
        // WHY: ディスク上の参照は保存時に "guid:<32hex>" へ変換されているため
        //      (GuidRefCodec)、ファイル名で探しても .scene からは 1 件も見つからない。
        //      一方 baked アセットや guid を持たない参照はパスのまま残るので、両方を見る。
        //
        // NOTE: 以前はファイル名に加えて「拡張子を除いた stem」でも一致とみなしていたが、
        //       これは "Fire" のような短い名前が無関係なファイルの本文へ大量に当たり、
        //       結果一覧が使い物にならなかった。stem 単独の一致は採らない。
        const std::string guid = asset::AssetDatabase::TryGetGuidFromPath(e.path);
        const std::string guidRef = guid.empty()
            ? std::string{} : std::string(asset::AssetDatabase::kGuidPrefix) + guid;
        // パス参照は "Assets/..." 起点で書かれる。
        const std::string relativePath = NormalizeAssetPath(e.path);

        for (const auto& p : util::FileSystem::ListFilesRecursive(
                util::FileSystem::PathFromUtf8(m_rootPath))) {
            const std::string scanPath = util::FileSystem::PathToUtf8(p);
            const std::string scanExt  = util::StringUtils::ToLower(
                util::FileSystem::GetExtension(scanPath));
            if (scanExt != ".scene" && scanExt != ".mat" && scanExt != ".prefab"
                && scanExt != ".animcontroller" && scanExt != ".vfx") continue;
            // 自分自身は参照元に数えない。
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
    // 横断検索の結果からその場所へ移動する。
    // WHY: 検索でアセットを見つけた後、周辺のファイルも見たいことが多い。
    //      Explorer を開かずにブラウザ内で辿れるようにする。
    if (IsGlobalSearchActive() && !e.isDir && ImGui::MenuItem("Go to Containing Folder")) {
        // GetDirectory は末尾に '/' を付けて返すため落とす (パス比較が壊れる)。
        std::string folder = util::FileSystem::GetDirectory(e.path);
        while (folder.size() > 1 && (folder.back() == '/' || folder.back() == '\\'))
            folder.pop_back();
        m_pendingNavigate = std::move(folder);
        // 移動先では検索を解除しないと、そのフォルダの中身が見えない。
        m_searchBuf[0] = '\0';
        m_searchResultsQuery.clear();
        m_searchResultsTypeFilter = -1;
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
        // 入力欄は拡張子ぶんの幅を空けて置き、拡張子はその右へ編集不可の文字として描く。
        // WHY: 単に編集させないだけだと「最終的にどんなファイル名になるのか」が見えない。
        //      並べて出すことで、固定されていることと結果の両方が一目で分かる。
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float extWidth = m_renameExtension.empty()
            ? 0.0f
            : ImGui::CalcTextSize(m_renameExtension.c_str()).x + spacing;
        // グリッドのセル幅は可変なので、名前が 1 文字も打てない幅にならないよう下限を設ける。
        const float inputWidth = (std::max)(m_iconSize - extWidth, 40.0f);

        ImGui::SetNextItemWidth(inputWidth);
        // WHY: バッファが拡張子を含まなくなったので、ImGui 既定の「フォーカス時に全選択」が
        //      そのまま望みの挙動 (名前部分だけ選択) になる。手動の範囲指定は不要。
        if (m_renameNeedFocus) { ImGui::SetKeyboardFocusHere(); m_renameNeedFocus = false; }
        constexpr ImGuiInputTextFlags renameFlags = ImGuiInputTextFlags_EnterReturnsTrue;
        const bool enterPressed = ImGui::InputText("##rename", m_renameBuffer,
                                                   sizeof(m_renameBuffer), renameFlags);
        // WHY: 直後に拡張子ラベルを描くと IsItemDeactivated() の対象がそちらへ移り、
        //      「他所をクリックしてリネームを中断する」経路が死ぬ。ここで確定させる。
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
                        // .meta サイドカーも一緒に動かし、GUID 索引を追随させる。
                        if (!MoveAssetWithSidecar(oldPath, newPath)) {
                            FBZZ_LOG_ERROR("Rename failed: %s -> %s", oldPath.c_str(), newPath.c_str());
                        } else {
                            // リネームも Undo 履歴には積まない (移動・生成・削除と同じ扱い)。
                            // ファイル名はディスクの状態であり、シーン編集の履歴に混ぜると
                            // Scene View の Ctrl+Z がアセットを勝手に改名することになる。
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
    // 遅延リネームタイマー: ダブルクリック判定後 0.5s 経過でリネーム開始
    if (!m_pendingRenamePath.empty() && m_pendingRenamePath == e.path) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_pendingRenamePath.clear();
        } else if (!e.isMount &&
                   static_cast<float>(ImGui::GetTime()) - m_pendingRenameTimer > 0.5f) {
            BeginRenameForPath(m_pendingRenamePath, &ctx);
            m_pendingRenamePath.clear();
        }
    }

    ImGui::PushID(e.path.c_str());

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  sz     = m_iconSize;

    ImGui::InvisibleButton("##icon", { sz, sz });
    const bool hov = ImGui::IsItemHovered();

    const bool primarySelected = !e.isDir && e.path == ctx.selectedAssetPath;
    const bool selected = primarySelected || m_selectedPaths.count(e.path) > 0;
    // 複数選択中は「主選択 = Inspector に出ている 1 件」だけを濃く描き分ける。
    // 単一選択のときは主選択かどうかを区別する意味がないので常に濃い表現にする。
    const bool multiSelection = m_selectedPaths.size() > 1;
    const bool emphasized     = primarySelected || !multiSelection;
    // フォーカスを失っている間は彩度を落とす (Unity の Project ウィンドウと同じ)。
    const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 tileMin = { origin.x - 4.0f, origin.y - 4.0f };
    const ImVec2 tileMax = { origin.x + sz + 4.0f, origin.y + sz + 22.0f };

    // サブアセット (FBX 内メッシュ / 画像内スプライト) と展開元の親を 1 本の帯で繋ぐ。
    // 帯は隣接タイルとセル間の中点で接合するため、左右へ bleed だけ伸ばす。
    if (band.active) {
        ui::DrawSubAssetBand(
            dl,
            { tileMin.x - (band.joinLeft  ? band.bleed : 0.0f), tileMin.y },
            { tileMax.x + (band.joinRight ? band.bleed : 0.0f), tileMax.y },
            band.isParent, band.OpenLeft(), band.OpenRight(), 5.0f);
    }

    ui::DrawTileSelection(dl, tileMin, tileMax, selected, hov, emphasized, panelFocused, 5.0f);

    // Ping: 参照欄クリックで飛んできた対象を短時間だけ光らせる。
    // WHY: 選択ハイライトだけだと、大量のタイルが並ぶ一覧の中で「今どれに飛ばされたのか」を
    //      目で拾えない。Unity の Project ウィンドウと同じく、数百 ms のフラッシュで視線を誘導する。
    if (!m_pingPath.empty() && m_pingPath == e.path) {
        constexpr float PING_DURATION = 1.2f;
        const float elapsed = static_cast<float>(ImGui::GetTime()) - m_pingStartTime;
        if (elapsed < 0.0f || elapsed > PING_DURATION) {
            m_pingPath.clear();
        } else {
            // 2 回明滅させてから消える。線形フェードだと「点いて消えた」だけで気づきにくい。
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

    // 名前欄: 選択中は面で塗って白文字にし、サムネイルの絵柄に左右されず読めるようにする。
    if (selected && m_renamingPath != e.path) {
        ui::DrawTileLabelPlate(dl,
                               { tileMin.x + 2.0f, origin.y + sz + 2.0f },
                               { tileMax.x - 2.0f, tileMax.y - 1.0f },
                               emphasized, panelFocused, 4.0f);
    }

    // ドラッグソース。フォルダも移動対象にし、左ペインのフォルダツリーへ直接整理できるようにする。
    if (!e.isMount && !e.isPackageAsset && ImGui::BeginDragDropSource()) {
        m_entryDragStarted = true;  // ドラッグ中はリリース時の選択変更を抑制
        const std::string payloadPath = ToAssetDragPayloadPath(e.path, ctx);
        ImGui::SetDragDropPayload("ASSET_PATH", payloadPath.c_str(), payloadPath.size() + 1);
        // ImGui payloadはプロセス境界を越えないため、同じdragを独立VFXEditor向けIPCでも追跡する。
        VFXEditorLauncher::TrackAssetDrag(ctx.projectRoot, payloadPath);
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::EndDragDropSource();
    }
    // ドロップターゲット (ディレクトリのみ)
    if (e.isDir && ImGui::BeginDragDropTarget()) {
        // SaveHierarchyPayloadAsPrefab が requestAssetBrowserRefresh を立てるため、ここで
        // RefreshDirectory() は呼ばない。DrawEntry の参照列を描画中に無効化してしまう。
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

    // FBX / Sprite Texture の ▶/▼ 三角クリックで展開トグル。
    if (hov && !e.isSubAsset && e.hasSubAssets &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // 当たり判定はチップ (角丸の下地) の大きさに合わせる。見た目より狭いと
        // 「押したのに開かない」が起きるため、DrawEntryBadges と同じ pad を使う。
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

    // FindRefs ポップアップは1つのエントリが最初にレンダリングされた後に開く
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
        // 「見つからない」を「使われていない」と読ませない。走査対象を必ず添える。
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
            if (ImGui::Selectable(label.c_str())) {
                // クリックで親フォルダへナビゲート
                m_pendingNavigate = util::FileSystem::GetDirectory(ref);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ref.c_str());
        }
        ImGui::EndChild();
    }

    ImGui::Separator();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ─── FBX 内容プレビュー (サブアセットアイコン) ────────────────────────────────

} // namespace fbzz::editor
