// FBZZ Engine
// AssetManager.cpp | fbzz::asset
// アセットロード・キャッシュ管理
// 新 API は Load<T>() テンプレートをヘッダーでインライン化。ここでは旧 API と Init を実装する。
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/AnimationImporter.hpp>
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
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Asset/SkeletonImporter.hpp>
#include <Engine/Asset/TerrainAsset.hpp>
#include <Engine/Asset/TerrainImporter.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <limits>
#include <utility>

namespace fbzz::asset {

namespace {

std::unique_ptr<Model> ConvertModelAssetToLegacyModel(std::unique_ptr<ModelAsset> asset)
{
    if (!asset || asset->lods.empty()) return nullptr;

    auto model = std::make_unique<Model>();
    LodLevel& lod0 = asset->lods[0];
    model->meshes.reserve(lod0.submeshes.size());
    model->materials.reserve(lod0.submeshes.size());

    for (SubmeshEntry& submesh : lod0.submeshes) {
        if (!submesh.mesh) continue;

        // WHY: LoadModel() は旧コンポーネント向けの所有モデルを返すため、
        //      新 .fzasset 表現の LOD0 だけを移譲し、マテリアルは別アセット束縛までの既定値で埋める。
        model->meshes.push_back(std::move(submesh.mesh));
        model->materials.push_back(std::make_unique<renderer::Material>());
    }

    model->skeleton = std::move(asset->skeleton);
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

    auto model = std::make_unique<Model>();
    model->meshes.push_back(std::move(mesh));
    model->materials.push_back(std::make_unique<renderer::Material>());
    return model;
}

} // namespace

// ── 静的メンバ定義 ─────────────────────────────────────────────────────────

renderer::ResourceManager* AssetManager::s_resources   = nullptr;
std::string                AssetManager::s_basePath     = "Assets/";
bool                       AssetManager::s_initialized  = false;

bool                       AssetManager::S_init() noexcept { return s_initialized; }
const std::string&         AssetManager::S_base() noexcept { return s_basePath; }
renderer::ResourceManager* AssetManager::S_res()  noexcept { return s_resources; }
static int                 s_flushGeneration             = 0;

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

std::string AssetManager::ResolvePath(const std::string& key, const std::string& basePath)
{
    if (IsAbsPath(key)) return key;

    std::string k = key;
    if (StartsWithCI(k, "Assets/")) k = k.substr(7);

    std::string full = basePath;
    if (!full.empty() && full.back() != '/') full.push_back('/');
    full += k;

    if (util::FileSystem::Exists(full)) return full;
    if (util::FileSystem::Exists(key))  return key;
    return full;
}

std::string AssetManager::ResolveAssetPath(const std::string& path)
{
    return ResolvePath(Normalize(path), s_basePath);
}

// ── Init / UnloadAll ──────────────────────────────────────────────────────

void AssetManager::Init(renderer::ResourceManager& resources, const std::string& basePath)
{
    assert(!s_initialized && "AssetManager::Init() must be called once");
    s_resources   = &resources;
    s_basePath    = Normalize(basePath);
    s_initialized = true;

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
}

void AssetManager::UnloadAll()
{
    // 新 API ストアをクリア
    AssetStore<ModelAsset>::Get().Clear();
    AssetStore<AnimationClip>::Get().Clear();
    AssetStore<MaterialAsset>::Get().Clear();
    AssetStore<AnimatorControllerAsset>::Get().Clear();
    AssetStore<TerrainAsset>::Get().Clear();
    AssetStore<TextureAsset>::Get().Clear();

    // 旧 API キャッシュをクリア
    s_models.clear();
    s_materials.clear();
    s_materialSlots.clear();
    s_materialSlots.emplace_back();
    s_materialFreeList.clear();
    s_resources   = nullptr;
    s_initialized = false;
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
    if (store.importer)
        asset = store.importer->Import(
            ResolvePath(key, S_base()),
            S_res());
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

void AssetManager::FlushFailed()
{
    FlushStore<ModelAsset>();
    FlushStore<AnimationClip>();
    FlushStore<MaterialAsset>();
    FlushStore<AnimatorControllerAsset>();
    FlushStore<TerrainAsset>();
    FlushStore<TextureAsset>();

    for (auto it = s_models.begin(); it != s_models.end(); )
        it = it->second ? ++it : s_models.erase(it);
    for (auto it = s_materials.begin(); it != s_materials.end(); )
        it = it->second.IsValid() ? ++it : s_materials.erase(it);
    ++s_flushGeneration;
}

int AssetManager::GetFlushGeneration() { return s_flushGeneration; }

// ── 旧 API: LoadModel ────────────────────────────────────────────────────

Model* AssetManager::LoadModel(const std::string& relativePath)
{
    assert(s_initialized);
    const std::string key = Normalize(relativePath);
    auto it = s_models.find(key);
    if (it != s_models.end()) return it->second.get();

    const std::string fullPath = ResolvePath(key, s_basePath);
    std::unique_ptr<Model> model;
    if (key.ends_with(".mesh")) {
        model = LoadFzMeshModel(fullPath, *s_resources);
    } else if (key.ends_with(".fzasset")) {
        ModelAssetImporter importer;
        model = ConvertModelAssetToLegacyModel(importer.Import(fullPath, s_resources));
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
