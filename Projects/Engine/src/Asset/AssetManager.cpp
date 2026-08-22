// FBZZ Engine
// AssetManager.cpp | fbzz::asset
// アセットロード・キャッシュ管理
// 新 API は Load<T>() テンプレートをヘッダーでインライン化。ここでは旧 API と Init を実装する。
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/AnimationImporter.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AnimCtrlImporter.hpp>
#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Asset/FzTerrainSerializer.hpp>
#include <Engine/Asset/IblAsset.hpp>
#include <Engine/Asset/IblImporter.hpp>
#include <Engine/Asset/ImageImporter.hpp>
#include <Engine/Asset/MatAssetImporter.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/ModelAssetImporter.hpp>
#include <Engine/Asset/ModelImporter.hpp>
#include <Engine/Asset/PhysicsMaterialAsset.hpp>
#include <Engine/Asset/PhysicsMaterialImporter.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Asset/SkeletonImporter.hpp>
#include <Engine/Asset/TerrainAsset.hpp>
#include <Engine/Asset/TerrainImporter.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <Windows.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace fbzz::asset {

namespace {

// SDKのEngine assetsはプロジェクト外に一つだけ保持する。
// GameHub起動時は環境変数、配布Standaloneはexe隣のEngineAssetsから解決する。
std::string DiscoverEngineAssetRoot()
{
    const DWORD required = GetEnvironmentVariableW(L"FBZZ_ENGINE_ASSET_ROOT", nullptr, 0);
    if (required > 1) {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            L"FBZZ_ENGINE_ASSET_ROOT", value.data(), required);
        if (written > 0 && written < required) {
            value.resize(written);
            return util::FileSystem::PathToUtf8(std::filesystem::path(value));
        }
    }

    std::wstring executable(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(
            nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (written == 0) return {};
        if (written < executable.size()) {
            executable.resize(written);
            break;
        }
        executable.resize(executable.size() * 2);
    }
    const std::filesystem::path executableDirectory =
        std::filesystem::path(executable).parent_path();
    const std::filesystem::path stagedCandidate = executableDirectory / L"EngineAssets";
    if (util::FileSystem::Exists(util::FileSystem::PathToUtf8(stagedCandidate)))
        return util::FileSystem::PathToUtf8(stagedCandidate);

    // SDK Editorは tools/<Config>/Editor にあり、共有assetはSDK root/share配下にある。
    const std::filesystem::path sdkCandidate =
        executableDirectory.parent_path().parent_path().parent_path()
        / L"share" / L"fbzz" / L"Assets";
    return util::FileSystem::Exists(util::FileSystem::PathToUtf8(sdkCandidate))
        ? util::FileSystem::PathToUtf8(sdkCandidate)
        : std::string{};
}

// 壊れた guid 参照は解決のたびに (= 毎フレーム) 通るため、同じ参照は 1 度だけ報告する。
// WHY: Console のリングバッファは 512 件しかなく、1 件が毎フレーム流れると
//      他のログを押し出して「エラーが出ている」こと自体が見えなくなる。
std::unordered_set<std::string>& BrokenRefReports()
{
    static std::unordered_set<std::string> reported;
    return reported;
}

bool ShouldReportBrokenRef(const std::string& ref)
{
    return BrokenRefReports().insert(ref).second;
}

std::unique_ptr<Model> ConvertModelAssetToLegacyModel(std::unique_ptr<ModelAsset> asset)
{
    if (!asset || asset->lods.empty()) return nullptr;

    auto model = std::make_unique<Model>();
    LodLevel& lod0 = asset->lods[0];
    model->meshes.reserve(lod0.submeshes.size());
    model->materials.reserve(lod0.submeshes.size());

    // submesh の添字 → model->meshes の添字。
    // WHY 対応表が要るか: 下のループは mesh を持たない submesh を飛ばすため、
    //     ModelNode::meshIndices (submesh 基準) をそのまま使うと 1 個ずれた
    //     メッシュを指す。ノードが別の部位を描く、という静かな壊れ方になる。
    constexpr uint32_t kDropped = UINT32_MAX;
    std::vector<uint32_t> submeshToMesh(lod0.submeshes.size(), kDropped);

    for (size_t si = 0; si < lod0.submeshes.size(); ++si) {
        SubmeshEntry& submesh = lod0.submeshes[si];
        if (!submesh.mesh) continue;

        // WHY: LoadModel() は旧コンポーネント向けの所有モデルを返すため、
        //      新 .fzasset 表現の LOD0 だけを移譲し、マテリアルは別アセット束縛までの既定値で埋める。
        submeshToMesh[si] = static_cast<uint32_t>(model->meshes.size());
        model->meshes.push_back(std::move(submesh.mesh));
        model->materials.push_back(std::make_unique<renderer::Material>());
    }

    model->skeleton = std::move(asset->skeleton);

    // ノード階層を meshes の添字系へ張り替えて引き継ぐ。
    model->nodes.reserve(asset->nodes.size());
    for (const ModelNode& source : asset->nodes) {
        ModelNode node = source;
        node.meshIndices.clear();
        for (const uint32_t submeshIndex : source.meshIndices) {
            if (submeshIndex >= submeshToMesh.size()) continue;
            const uint32_t meshIndex = submeshToMesh[submeshIndex];
            if (meshIndex != kDropped) node.meshIndices.push_back(meshIndex);
        }
        model->nodes.push_back(std::move(node));
    }
    model->rootNodeIndex       = asset->nodes.empty() ? -1 : asset->rootNodeIndex;
    model->nodeTransformsBaked = asset->nodeTransformsBaked;

    // ベイクが v3 以前でノード情報が無い場合は、全メッシュを担当する
    // ルート 1 個として表現する。配置側にノード有無の分岐を書かせない。
    if (model->nodes.empty() && !model->meshes.empty()) {
        ModelNode root;
        root.name = "Mesh";
        root.meshIndices.reserve(model->meshes.size());
        for (uint32_t i = 0; i < static_cast<uint32_t>(model->meshes.size()); ++i)
            root.meshIndices.push_back(i);
        model->nodes.push_back(std::move(root));
        model->rootNodeIndex       = 0;
        model->nodeTransformsBaked = true;
    }

    return model;
}

std::unique_ptr<Model> LoadFzMeshModel(
    const std::string& absPath,
    renderer::ResourceManager& resources)
{
    BinaryReader reader;
    if (!reader.Open(absPath)) {
        FBZZ_LOG_ERROR("AssetManager: cannot open .mesh [%s]", absPath.c_str());
        return nullptr;
    }

    FzMeshHeader hdr{};
    if (!reader.Read(hdr) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'M' || hdr.magic[3] != 'H') {
        FBZZ_LOG_ERROR("AssetManager: bad .mesh magic [%s]", absPath.c_str());
        return nullptr;
    }
    if (hdr.version != FZMESH_VERSION || hdr.vertexCount == 0 || hdr.indexCount == 0) {
        FBZZ_LOG_ERROR("AssetManager: unsupported .mesh header version=%u vertices=%u indices=%u [%s]",
                       hdr.version, hdr.vertexCount, hdr.indexCount, absPath.c_str());
        return nullptr;
    }

    auto mesh = std::make_unique<renderer::Mesh>();
    mesh->vertexCount = hdr.vertexCount;
    mesh->indexCount = hdr.indexCount;
    mesh->boundsCenter = { hdr.boundsCenter[0], hdr.boundsCenter[1], hdr.boundsCenter[2] };
    mesh->boundsRadius = hdr.boundsRadius;
    mesh->isSkinned = (hdr.flags & FZMESH_FLAG_SKINNED) != 0u;

    if (mesh->isSkinned) {
        mesh->cpuSkinnedVertices.resize(hdr.vertexCount);
        if (!reader.ReadBytes(mesh->cpuSkinnedVertices.data(),
                              hdr.vertexCount * sizeof(renderer::SkinnedVertex))) {
            FBZZ_LOG_ERROR("AssetManager: truncated .mesh skinned vertices [%s]", absPath.c_str());
            return nullptr;
        }
        mesh->vertexBuffer = resources.CreateVertexBuffer(
            mesh->cpuSkinnedVertices.data(),
            mesh->cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex),
            sizeof(renderer::SkinnedVertex));
    } else {
        mesh->cpuVertices.resize(hdr.vertexCount);
        if (!reader.ReadBytes(mesh->cpuVertices.data(),
                              hdr.vertexCount * sizeof(renderer::Vertex))) {
            FBZZ_LOG_ERROR("AssetManager: truncated .mesh vertices [%s]", absPath.c_str());
            return nullptr;
        }
        mesh->vertexBuffer = resources.CreateVertexBuffer(
            mesh->cpuVertices.data(),
            mesh->cpuVertices.size() * sizeof(renderer::Vertex),
            sizeof(renderer::Vertex));
    }

    mesh->cpuIndices.resize(hdr.indexCount);
    if (!reader.ReadBytes(mesh->cpuIndices.data(), hdr.indexCount * sizeof(uint32_t))) {
        FBZZ_LOG_ERROR("AssetManager: truncated .mesh indices [%s]", absPath.c_str());
        return nullptr;
    }
    mesh->indexBuffer = resources.CreateIndexBuffer(mesh->cpuIndices.data(), hdr.indexCount);
    // ヘッダーは球しか持たないので AABB だけ頂点から補う (遮蔽者に使えるかの判定材料)。
    mesh->ComputeBoundsExtents();

    auto model = std::make_unique<Model>();
    model->meshes.push_back(std::move(mesh));
    model->materials.push_back(std::make_unique<renderer::Material>());

    // .mesh は単一メッシュを結合済みで持つ形式なので、階層は常にルート 1 個。
    // 配置側にノード有無の分岐を持たせないため、その 1 個を明示的に作る。
    ModelNode root;
    root.name = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(absPath).stem());
    if (root.name.empty()) root.name = "Mesh";
    root.meshIndices.push_back(0u);
    model->nodes.push_back(std::move(root));
    model->rootNodeIndex = 0;
    return model;
}

} // namespace

// ── 静的メンバ定義 ─────────────────────────────────────────────────────────

renderer::ResourceManager* AssetManager::s_resources   = nullptr;
std::string                AssetManager::s_basePath     = "Assets/";
std::string                AssetManager::s_engineBasePath;
bool                       AssetManager::s_initialized  = false;

bool                       AssetManager::S_init() noexcept { return s_initialized; }
const std::string&         AssetManager::S_base() noexcept { return s_basePath; }
renderer::ResourceManager* AssetManager::S_res()  noexcept { return s_resources; }
static int                 s_flushGeneration             = 0;
// 中身が変わるたびに進む。失敗キャッシュの掃除 (FlushFailed) と
// ファイル変更による差し替え (ReloadPath) の両方で進める。
static int                 s_assetGeneration             = 0;

std::unordered_map<std::string, std::unique_ptr<Model>>
    AssetManager::s_models;
std::vector<AssetManager::MatSlot>
    AssetManager::s_materialSlots;
std::vector<uint32_t>
    AssetManager::s_materialFreeList;
std::unordered_map<std::string, renderer::ResourceHandle<renderer::MaterialAssetTag>>
    AssetManager::s_materials;

// ── パスユーティリティ ────────────────────────────────────────────────────

std::string AssetManager::Normalize(const std::string& path)
{
    std::string r = path;
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

static bool StartsWithCI(const std::string& s, const char* prefix)
{
    for (size_t i = 0; prefix[i]; ++i) {
        if (i >= s.size()) return false;
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    }
    return true;
}

static bool IsAbsPath(const std::string& p)
{
    if (p.empty()) return false;
    if (p[0] == '/') return true;
    return p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) &&
           p[1] == ':' && p[2] == '/';
}

static bool EndsWithCI(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
    }
    return true;
}

// import 生成物 (baked) の拡張子か。これらは Assets ではなく Library/Baked に置かれる。
//
// WHY .anim を含めるか: クリップは FBX から毎回焼き直される派生物で、編集対象となる設定
//     (Loop Time 等) は原本の .fbx.meta 側にある。Assets に置いておく理由が無く、
//     置いたままだと 1 モデルにつき数十ファイルがブラウザーを埋める。
//     取り出して独立編集したい場合は Extract で Assets へ複製する (新 GUID が振られる)。
// WHY .mat を含めても手作りマテリアルが壊れないか:
//     LibraryBakedPath は「パッケージ規約 (Foo/ の隣に原本 Foo.fbx)」が成立しない限り
//     空を返す。Assets/Materials/*.mat のような手作りはここで必ず外れ、
//     さらに上の "Assets 優先" 分岐で先に実体が返るため、2 重に守られている。
static bool IsBakedModelExt(const std::string& key)
{
    return EndsWithCI(key, ".fzasset") || EndsWithCI(key, ".mesh") ||
           EndsWithCI(key, ".skel")    || EndsWithCI(key, ".anim") ||
           EndsWithCI(key, ".mat");
}

// Extract で Assets 側へ取り出せる生成物か。取り出した実体があればそちらを優先する。
// 画像は "textures/" 配下に居るときだけ生成物なので、拡張子だけでは判定しない
// (手持ちのテクスチャまで Library を探しに行かせない)。
static bool IsExtractableBaked(const std::string& key)
{
    static constexpr const char* kImageExts[] = {
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp", ".hdr", ".exr"
    };
    if (EndsWithCI(key, ".anim") || EndsWithCI(key, ".mat")) return true;
    for (const char* ext : kImageExts)
        if (EndsWithCI(key, ext)) return true;
    return false;
}

// 論理パス → 物理パス "<projectRoot>/Library/Baked/<fbx-guid>/[anims/]<file>"。
//
// 扱う 2 形:
//   "<dir>/Foo/Foo.fzasset"            → Library/Baked/<guid>/Foo.fzasset
//   "<dir>/Foo/anims/Idle.anim"        → Library/Baked/<guid>/anims/Idle.anim
//
// パッケージ規約 (Foo/ の隣に原本 Foo.fbx) から fbx を特定し、その guid でキャッシュを引く。
// fbx が存在しない / basePath が "Assets/" で終わらない場合は空を返す (呼び出し側がフォールバック)。
static std::string LibraryBakedPath(const std::string& absPath, const std::string& basePath)
{
    namespace fs = std::filesystem;
    if (!EndsWithCI(basePath, "assets/")) return {};

    const fs::path p = util::FileSystem::PathFromUtf8(absPath);
    fs::path pkgDir  = p.parent_path();

    // anims/ と materials/ は 1 階層深い。Library 側でも同じサブディレクトリを維持することで、
    // 同名クリップ・同名マテリアルを持つ別 FBX どうしがぶつからない。
    std::string subDir;
    {
        std::string leaf = util::FileSystem::PathToUtf8(pkgDir.filename());
        std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (leaf == "anims" || leaf == "materials" || leaf == "textures") {
            subDir = leaf + "/";
            pkgDir = pkgDir.parent_path();
        }
    }

    const std::string pkg  = util::FileSystem::PathToUtf8(pkgDir.filename());
    const fs::path fbxPath = pkgDir.parent_path() / (pkg + ".fbx");

    // GuidFromPath は実在チェック込み。fbx が無ければ空 = Library 対象外。
    const std::string guid = AssetDatabase::TryGetGuidFromPath(
        util::FileSystem::PathToUtf8(fbxPath));
    if (guid.empty()) return {};

    const std::string projectRoot = basePath.substr(0, basePath.size() - 7); // "assets/" を除去
    return projectRoot + "Library/Baked/" + guid + "/" + subDir +
           util::FileSystem::PathToUtf8(p.filename());
}

// 原本 FBX の恒久 GUID から Library/Baked 内のモデルコンテナを引く。
// WHY: Scene / Component には Unity と同じく原本 .fbx を保存し、再生成可能な .fzasset は
//      AssetBrowser や Inspector に露出しない内部キャッシュとして扱うため。
static std::string LibraryBakedModelPathFromFbx(const std::string& fbxAbsPath, const std::string& basePath)
{
    namespace fs = std::filesystem;
    if (!EndsWithCI(basePath, "assets/")) return {};

    const std::string guid = AssetDatabase::TryGetGuidFromPath(fbxAbsPath);
    if (guid.empty()) return {};

    const fs::path fbxPath = util::FileSystem::PathFromUtf8(fbxAbsPath);
    const std::string projectRoot = basePath.substr(0, basePath.size() - 7); // "assets/" を除去
    return projectRoot + "Library/Baked/" + guid + "/" +
           util::FileSystem::PathToUtf8(fbxPath.stem()) + ".fzasset";
}

static std::string ResolveModelAssetPath(const std::string& key, const std::string& basePath)
{
    namespace fs = std::filesystem;
    if (!EndsWithCI(key, ".fbx")) return AssetManager::ResolveAssetPath(key);

    std::string fbxFull;
    if (IsAbsPath(key)) {
        fbxFull = key;
    } else {
        std::string k = key;
        if (StartsWithCI(k, "Assets/")) k = k.substr(7);
        fbxFull = basePath;
        if (!fbxFull.empty() && fbxFull.back() != '/') fbxFull.push_back('/');
        fbxFull += k;
    }

    const std::string lib = LibraryBakedModelPathFromFbx(fbxFull, basePath);
    if (!lib.empty() && util::FileSystem::Exists(lib)) return lib;

    return fbxFull;
}

std::string AssetManager::ResolvePath(const std::string& key, const std::string& basePath)
{
    // "guid:<32hex>" 参照は AssetDatabase で実パスへ解決する。
    // WHY: 全ロードがこの一点を通るため、ここに分岐を置くだけで
    //      .mat / .scene / コンポーネントの GUID 参照がエンジン全体で有効になる。
    if (AssetDatabase::IsGuidRef(key)) {
        // GuidFromRef が併記されたパスヒントとサブアセット接尾辞を落とす。
        std::string p = AssetDatabase::PathFromGuid(AssetDatabase::GuidFromRef(key));
        // 索引を引けても実体が消えていれば参照は切れている。どちらも同じ「パス切れ」として報告する。
        if ((p.empty() || !util::FileSystem::Exists(p)) && ShouldReportBrokenRef(key)) {
            const std::string hint = AssetDatabase::HintFromRef(key);
            FBZZ_LOG_ERROR("AssetManager: broken guid reference [%s]%s%s",
                           key.c_str(),
                           hint.empty() ? "" : "\n  last known path: ",
                           hint.c_str());
        }
        return p;
    }

    // まず論理パスを絶対化する (従来と同じ規則)。
    std::string full;
    if (IsAbsPath(key)) {
        full = key;
    } else {
        std::string k = key;
        if (StartsWithCI(k, "Assets/")) k = k.substr(7);
        full = basePath;
        if (!full.empty() && full.back() != '/') full.push_back('/');
        full += k;
    }

    // baked 生成物は Library/Baked/<fbx-guid>/ から解決する。
    //
    // WHY .anim / .mat だけ Assets を先に見るか (Extract の仕組み):
    //   これらは AssetBrowser の "Extract to Assets" で Assets 側へ取り出せる。
    //   取り出した実体があるならそちらが人の編集を受けた正であり、
    //   Library の再生成物より優先されなければならない。取り出していなければ
    //   Assets 側は存在しないので、そのまま Library へ落ちる。
    //   .fzasset / .mesh / .skel は取り出しの対象外なので常に Library が先。
    if (IsBakedModelExt(key) || IsExtractableBaked(key)) {
        if (IsExtractableBaked(key) && util::FileSystem::Exists(full)) return full;

        const std::string lib = LibraryBakedPath(full, basePath);
        if (!lib.empty() && util::FileSystem::Exists(lib)) return lib;
    }

    // 開いた Scene / Prefab がリネーム前の論理パスを一時的に保持していても、
    // AssetDatabase の移動 alias から現在の実体へ追従させる。
    // WHY: 保存時だけ GUID 化できても、リネーム直後のプレビューや Inspector の
    //      実体ロードが旧パスのままだと「参照が切れた」と見えて編集を続けられない。
    if (const std::string movedGuid = AssetDatabase::TryGetGuidFromPath(full);
        !movedGuid.empty()) {
        const std::string movedPath = AssetDatabase::PathFromGuid(movedGuid);
        if (!movedPath.empty() && movedPath != full
            && util::FileSystem::Exists(movedPath))
            return movedPath;
    }

    if (IsAbsPath(key)) return key;
    if (util::FileSystem::Exists(full)) return full;
    if (!s_engineBasePath.empty()) {
        std::string enginePath = s_engineBasePath;
        if (enginePath.back() != '/') enginePath.push_back('/');
        std::string relative = key;
        if (StartsWithCI(relative, "Assets/")) relative = relative.substr(7);
        enginePath += relative;
        if (util::FileSystem::Exists(enginePath)) return enginePath;
    }
    if (util::FileSystem::Exists(key))  return key;
    return full;
}

std::string AssetManager::ResolveAssetPath(const std::string& path)
{
    return ResolvePath(Normalize(path), s_basePath);
}

std::string AssetManager::BakedDirForSource(const std::string& sourceAbsPath)
{
    if (!EndsWithCI(s_basePath, "assets/")) return {};

    const std::string guid = AssetDatabase::TryGetGuidFromPath(Normalize(sourceAbsPath));
    if (guid.empty()) return {};

    const std::string projectRoot = s_basePath.substr(0, s_basePath.size() - 7);
    return projectRoot + "Library/Baked/" + guid;
}

// ── Init / UnloadAll ──────────────────────────────────────────────────────

void AssetManager::Init(renderer::ResourceManager& resources, const std::string& basePath)
{
    assert(!s_initialized && "AssetManager::Init() must be called once");
    s_resources   = &resources;
    s_basePath    = Normalize(basePath);
    s_engineBasePath = Normalize(DiscoverEngineAssetRoot());
    s_initialized = true;
    // プロジェクトを切り替えたら、前プロジェクトで報告済みの参照は関係なくなる。
    BrokenRefReports().clear();

    // GUID ⇄ パス索引を構築する (.meta の自己修復もここで走る)。
    // ResolvePath の "guid:" 分岐が使う前提なので、importer 登録より先に済ませる。
    AssetDatabase::Init(s_basePath);

    // slot 0 は null 予約
    if (s_materialSlots.empty()) s_materialSlots.emplace_back();

    // ── 新 API: インポーター登録 ─────────────────────────────────────
    RegisterImporter<ModelAsset>              (std::make_unique<ModelAssetImporter>());
    RegisterImporter<AnimationClip>           (std::make_unique<AnimationImporter>());
    RegisterImporter<Skeleton>                (std::make_unique<SkeletonImporter>());
    RegisterImporter<MaterialAsset>           (std::make_unique<MatAssetImporter>());
    RegisterImporter<AnimatorControllerAsset> (std::make_unique<AnimCtrlImporter>());
    RegisterImporter<TerrainAsset>            (std::make_unique<TerrainImporter>());
    RegisterImporter<TextureAsset>            (std::make_unique<ImageImporter>());
    RegisterImporter<IblAsset>               (std::make_unique<IblImporter>());
    RegisterImporter<PhysicsMaterialAsset>    (std::make_unique<PhysicsMaterialImporter>());
}

void AssetManager::UnloadAll()
{
    // 新 API ストアをクリア
    // WHY 全型を漏れなく並べるか: 消し忘れた型はプロジェクトを切り替えても
    //     前のプロジェクトのアセットが cache に残り続ける。パスが同名なら
    //     別プロジェクトの中身が黙って引き当たるという、最も気付きにくい壊れ方をする。
    AssetStore<ModelAsset>::Get().Clear();
    AssetStore<AnimationClip>::Get().Clear();
    AssetStore<Skeleton>::Get().Clear();
    AssetStore<MaterialAsset>::Get().Clear();
    AssetStore<AnimatorControllerAsset>::Get().Clear();
    AssetStore<TerrainAsset>::Get().Clear();
    AssetStore<TextureAsset>::Get().Clear();
    AssetStore<IblAsset>::Get().Clear();
    AssetStore<PhysicsMaterialAsset>::Get().Clear();

    // 旧 API キャッシュをクリア
    s_models.clear();
    s_materials.clear();
    s_materialSlots.clear();
    s_materialSlots.emplace_back();
    s_materialFreeList.clear();
    s_resources   = nullptr;
    s_engineBasePath.clear();
    s_initialized = false;
    BrokenRefReports().clear();

    // GUID 索引もアセットと同じライフサイクルで破棄する (再 Init で再構築)。
    AssetDatabase::Shutdown();
}

// ── FlushFailed ──────────────────────────────────────────────────────────

// ストアの null ハンドルエントリを削除してインポート再試行を可能にする
template<typename T>
static void FlushStore() {
    auto& store = AssetStore<T>::Get();
    for (auto it = store.cache.begin(); it != store.cache.end(); ) {
        if (!it->second.IsValid()) it = store.cache.erase(it);
        else ++it;
    }
}

template<typename T>
AssetHandle<T> AssetManager::LoadFromStore(const std::string& relativePath)
{
    assert(S_init() && "AssetManager::Init() must be called first");
    const std::string key = Normalize(relativePath);
    AssetStore<T>& store = AssetStore<T>::Get();

    const auto it = store.cache.find(key);
    if (it != store.cache.end()) return it->second;

    std::unique_ptr<T> asset;
    if (store.importer) {
        const std::string importPath = [&] {
            if constexpr (std::is_same_v<T, ModelAsset>)
                return ResolveModelAssetPath(key, S_base());
            else
                return ResolvePath(key, S_base());
        }();
        asset = store.importer->Import(importPath, S_res());
    }
    else
        FBZZ_LOG_ERROR("AssetManager: importer is not registered [%s]", key.c_str());

    if (!asset) {
        store.cache[key] = AssetHandle<T>::Null();
        return AssetHandle<T>::Null();
    }
    const AssetHandle<T> h = store.Alloc(std::move(asset));
    store.cache[key] = h;
    return h;
}

template<typename T>
T* AssetManager::GetFromStore(AssetHandle<T> h)
{
    return AssetStore<T>::Get().GetPtr(h);
}

template<typename T>
void AssetManager::UnloadFromStore(const std::string& relativePath)
{
    const std::string key = Normalize(relativePath);
    AssetStore<T>& store = AssetStore<T>::Get();
    const auto it = store.cache.find(key);
    if (it == store.cache.end()) return;
    store.Free(it->second);
    store.cache.erase(it);
}

template<>
AssetHandle<ModelAsset> AssetManager::Load<ModelAsset>(const std::string& relativePath)
{
    return LoadFromStore<ModelAsset>(relativePath);
}

template<>
AssetHandle<AnimationClip> AssetManager::Load<AnimationClip>(const std::string& relativePath)
{
    return LoadFromStore<AnimationClip>(relativePath);
}

template<>
AssetHandle<MaterialAsset> AssetManager::Load<MaterialAsset>(const std::string& relativePath)
{
    return LoadFromStore<MaterialAsset>(relativePath);
}

template<>
AssetHandle<AnimatorControllerAsset> AssetManager::Load<AnimatorControllerAsset>(const std::string& relativePath)
{
    return LoadFromStore<AnimatorControllerAsset>(relativePath);
}

template<>
AssetHandle<TerrainAsset> AssetManager::Load<TerrainAsset>(const std::string& relativePath)
{
    return LoadFromStore<TerrainAsset>(relativePath);
}

template<>
AssetHandle<TextureAsset> AssetManager::Load<TextureAsset>(const std::string& relativePath)
{
    return LoadFromStore<TextureAsset>(relativePath);
}

template<>
AssetHandle<PhysicsMaterialAsset> AssetManager::Load<PhysicsMaterialAsset>(const std::string& relativePath)
{
    return LoadFromStore<PhysicsMaterialAsset>(relativePath);
}

template<>
ModelAsset* AssetManager::Get<ModelAsset>(AssetHandle<ModelAsset> h)
{
    return GetFromStore<ModelAsset>(h);
}

template<>
AnimationClip* AssetManager::Get<AnimationClip>(AssetHandle<AnimationClip> h)
{
    return GetFromStore<AnimationClip>(h);
}

template<>
MaterialAsset* AssetManager::Get<MaterialAsset>(AssetHandle<MaterialAsset> h)
{
    return GetFromStore<MaterialAsset>(h);
}

template<>
AnimatorControllerAsset* AssetManager::Get<AnimatorControllerAsset>(AssetHandle<AnimatorControllerAsset> h)
{
    return GetFromStore<AnimatorControllerAsset>(h);
}

template<>
TerrainAsset* AssetManager::Get<TerrainAsset>(AssetHandle<TerrainAsset> h)
{
    return GetFromStore<TerrainAsset>(h);
}

template<>
TextureAsset* AssetManager::Get<TextureAsset>(AssetHandle<TextureAsset> h)
{
    return GetFromStore<TextureAsset>(h);
}

template<>
PhysicsMaterialAsset* AssetManager::Get<PhysicsMaterialAsset>(AssetHandle<PhysicsMaterialAsset> h)
{
    return GetFromStore<PhysicsMaterialAsset>(h);
}

template<>
void AssetManager::Unload<ModelAsset>(const std::string& relativePath)
{
    UnloadFromStore<ModelAsset>(relativePath);
}

template<>
void AssetManager::Unload<AnimationClip>(const std::string& relativePath)
{
    UnloadFromStore<AnimationClip>(relativePath);
}

template<>
void AssetManager::Unload<MaterialAsset>(const std::string& relativePath)
{
    UnloadFromStore<MaterialAsset>(relativePath);
}

template<>
void AssetManager::Unload<AnimatorControllerAsset>(const std::string& relativePath)
{
    UnloadFromStore<AnimatorControllerAsset>(relativePath);
}

template<>
void AssetManager::Unload<TerrainAsset>(const std::string& relativePath)
{
    UnloadFromStore<TerrainAsset>(relativePath);
}

template<>
void AssetManager::Unload<TextureAsset>(const std::string& relativePath)
{
    UnloadFromStore<TextureAsset>(relativePath);
}

template<>
void AssetManager::Unload<PhysicsMaterialAsset>(const std::string& relativePath)
{
    UnloadFromStore<PhysicsMaterialAsset>(relativePath);
}

void AssetManager::FlushFailed()
{
    // WHY Init() のインポーター登録と同じ並び・同じ顔ぶれで書くか:
    //     ここに 1 つ書き忘れると、その型は「一度ロードに失敗したら
    //     プロセスが終わるまで二度と復帰しない」という無音の不具合になる。
    //     LoadFromStore() は失敗も cache へ焼き付けるため、掃除口はここしかない。
    //     実際 .physmat は登録漏れで、アセットを直しても参照が復活しなかった。
    FlushStore<ModelAsset>();
    FlushStore<AnimationClip>();
    FlushStore<Skeleton>();
    FlushStore<MaterialAsset>();
    FlushStore<AnimatorControllerAsset>();
    FlushStore<TerrainAsset>();
    FlushStore<TextureAsset>();
    FlushStore<IblAsset>();
    FlushStore<PhysicsMaterialAsset>();

    for (auto it = s_models.begin(); it != s_models.end(); )
        it = it->second ? ++it : s_models.erase(it);
    for (auto it = s_materials.begin(); it != s_materials.end(); )
        it = it->second.IsValid() ? ++it : s_materials.erase(it);
    ++s_flushGeneration;
    ++s_assetGeneration;
}

int AssetManager::GetFlushGeneration() { return s_flushGeneration; }
int  AssetManager::GetAssetGeneration()  { return s_assetGeneration; }
void AssetManager::BumpAssetGeneration() { ++s_assetGeneration; }

// 監視イベントの絶対パスと、キャッシュキーを解決した実パスを同じ土俵で比べる。
//
// WHY 素の == で足りないか: キャッシュキーは "guid:..." / "Assets/..." / 絶対パスが
//     混在し、解決結果も区切り文字と大小がまちまちになる。文字列一致だけで判定すると、
//     同じファイルなのに再読込されない取りこぼしが経路ごとに出る。
static bool SameFilePathCI(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    const auto fold = [](char c) {
        if (c == '\\') return '/';
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (size_t i = 0; i < a.size(); ++i)
        if (fold(a[i]) != fold(b[i])) return false;
    return true;
}

template<typename T>
int AssetManager::ReloadFromStore(const std::string& absPath)
{
    AssetStore<T>& store = AssetStore<T>::Get();
    if (!store.importer) return 0;

    int reloaded = 0;
    for (auto it = store.cache.begin(); it != store.cache.end(); ) {
        // WHY 失敗した guid 参照を先に外すか: ResolvePath は解決できない guid に
        //     対してエラーログを出す。ここはファイルが変わるたびに全キャッシュを
        //     走査するので、触ると 1 回の保存でログが件数分あふれる。
        //     参照が切れたままの項目の掃除は FlushFailed の役目。
        if (!it->second.IsValid() && AssetDatabase::IsGuidRef(it->first)) { ++it; continue; }

        const std::string resolved = ResolvePath(it->first, s_basePath);
        if (!SameFilePathCI(resolved, absPath)) { ++it; continue; }

        // 失敗として焼き付いている項目は、消しておけば次の Load が取り直す。
        if (!it->second.IsValid()) {
            it = store.cache.erase(it);
            ++reloaded;
            continue;
        }

        std::unique_ptr<T> fresh = store.importer->Import(resolved, s_resources);
        if (!fresh) {
            // 書き込み途中のファイルを掴んだ可能性がある。動いている中身を捨てない。
            FBZZ_LOG_WARN("AssetManager: reload failed, keeping previous content [%s]",
                          it->first.c_str());
            ++it;
            continue;
        }
        if (store.Replace(it->second, std::move(fresh))) ++reloaded;
        ++it;
    }
    return reloaded;
}

int AssetManager::ReloadPath(const std::string& absPath)
{
    if (!s_initialized || absPath.empty()) return 0;
    const std::string target = Normalize(absPath);

    // WHY GPU 資源を持つ型を並べないか: TextureAsset / IblAsset は gpuHandle を素で持ち、
    //     デストラクタで解放しない。差し替えると前の版の GPU テクスチャが解放されないまま
    //     residual になる。画像・モデルは原本の自動再インポート経路が別にあるので、
    //     ここでは「編集されたら中身がそのまま変わる」型だけを扱う。
    int reloaded = 0;
    reloaded += ReloadFromStore<AnimationClip>(target);
    reloaded += ReloadFromStore<AnimatorControllerAsset>(target);
    reloaded += ReloadFromStore<MaterialAsset>(target);
    reloaded += ReloadFromStore<PhysicsMaterialAsset>(target);
    reloaded += ReloadFromStore<TerrainAsset>(target);

    // 旧 API の .mat は別のスロットプールに載る。レンダラーが参照しているのは
    // こちらなので、新 API 側だけ差し替えても画面は古いままになる。
    for (auto it = s_materials.begin(); it != s_materials.end(); ) {
        if (!it->second.IsValid() && AssetDatabase::IsGuidRef(it->first)) { ++it; continue; }

        const std::string resolved = ResolvePath(it->first, s_basePath);
        if (!SameFilePathCI(resolved, target)) { ++it; continue; }
        if (!it->second.IsValid()) {
            it = s_materials.erase(it);
            ++reloaded;
            continue;
        }
        auto mat = std::make_unique<MaterialAsset>();
        if (!LoadMaterialAssetFromFile(resolved, *mat)) {
            FBZZ_LOG_WARN("AssetManager: material reload failed, keeping previous [%s]",
                          it->first.c_str());
            ++it;
            continue;
        }
        if (IsMaterialLive(it->second)) {
            s_materialSlots[it->second.id].asset = std::move(mat);
            ++reloaded;
        }
        ++it;
    }

    if (reloaded > 0) {
        ++s_assetGeneration;
        FBZZ_LOG_INFO("AssetManager: reloaded %d asset(s) from [%s]",
                      reloaded, target.c_str());
    }
    return reloaded;
}

// ── 旧 API: LoadModel ────────────────────────────────────────────────────

Model* AssetManager::LoadModel(const std::string& relativePath)
{
    assert(s_initialized);
    const std::string key = Normalize(relativePath);
    auto it = s_models.find(key);
    if (it != s_models.end()) return it->second.get();

    const std::string fullPath = ResolvePath(key, s_basePath);
    std::unique_ptr<Model> model;
    if (EndsWithCI(key, ".mesh")) {
        model = LoadFzMeshModel(fullPath, *s_resources);
    } else if (EndsWithCI(key, ".fzasset") || EndsWithCI(key, ".fbx")) {
        const std::string modelAssetPath = EndsWithCI(key, ".fbx")
            ? ResolveModelAssetPath(key, s_basePath)
            : fullPath;
        ModelAssetImporter importer;
        model = ConvertModelAssetToLegacyModel(importer.Import(modelAssetPath, s_resources));
    } else {
        model = ModelImporter::Import(fullPath, *s_resources);
    }

    if (!model) {
        FBZZ_LOG_WARN("AssetManager: Model load failed [%s]", fullPath.c_str());
        s_models[key] = nullptr;
        return nullptr;
    }
    Model* ptr = model.get();
    s_models[key] = std::move(model);
    return ptr;
}


// ── 旧 API: MaterialAsset スロットプール ─────────────────────────────────

bool AssetManager::IsMaterialLive(renderer::ResourceHandle<renderer::MaterialAssetTag> h)
{
    if (!h.IsValid() || h.id >= s_materialSlots.size()) return false;
    const MatSlot& s = s_materialSlots[h.id];
    return s.occupied && s.gen == h.gen;
}

renderer::ResourceHandle<renderer::MaterialAssetTag>
AssetManager::AllocMaterialSlot(std::unique_ptr<MaterialAsset> asset)
{
    uint32_t id = 0;
    if (!s_materialFreeList.empty()) {
        id = s_materialFreeList.back();
        s_materialFreeList.pop_back();
    } else {
        id = static_cast<uint32_t>(s_materialSlots.size());
        s_materialSlots.emplace_back();
    }
    MatSlot& slot  = s_materialSlots[id];
    slot.asset     = std::move(asset);
    slot.occupied  = true;
    return { id, slot.gen };
}

renderer::ResourceHandle<renderer::MaterialAssetTag>
AssetManager::LoadMaterial(const std::string& relativePath)
{
    assert(s_initialized);
    const std::string key = Normalize(relativePath);
    auto it = s_materials.find(key);
    if (it != s_materials.end()) return it->second;

    auto mat = std::make_unique<MaterialAsset>();
    if (!LoadMaterialAssetFromFile(ResolvePath(key, s_basePath), *mat)) {
        FBZZ_LOG_WARN("AssetManager: MaterialAsset load failed [%s]", key.c_str());
        s_materials[key] = renderer::ResourceHandle<renderer::MaterialAssetTag>::Null();
        return {};
    }
    const auto h = AllocMaterialSlot(std::move(mat));
    s_materials[key] = h;
    return h;
}

MaterialAsset* AssetManager::GetMaterial(renderer::ResourceHandle<renderer::MaterialAssetTag> h)
{
    if (!IsMaterialLive(h)) return nullptr;
    return s_materialSlots[h.id].asset.get();
}

void AssetManager::UnloadMaterial(const std::string& relativePath)
{
    const std::string key = Normalize(relativePath);
    auto it = s_materials.find(key);
    if (it == s_materials.end()) return;
    const auto h = it->second;
    s_materials.erase(it);
    if (!IsMaterialLive(h)) return;
    MatSlot& slot = s_materialSlots[h.id];
    slot.asset.reset();
    slot.occupied = false;
    slot.gen = (slot.gen == std::numeric_limits<uint32_t>::max()) ? 1u : slot.gen + 1u;
    s_materialFreeList.push_back(h.id);
}

} // namespace fbzz::asset
