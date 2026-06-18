// FBZZ Engine
// AssetManager.cpp | fbzz::asset
// アセットロード・キャッシュ管理
// 新 API は Load<T>() テンプレートをヘッダーでインライン化。ここでは旧 API と Init を実装する。
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AnimCtrlImporter.hpp>
#include <Engine/Asset/FzAnimImporter.hpp>
#include <Engine/Asset/FzAssetLoader.hpp>
#include <Engine/Asset/FzModelImporter.hpp>
#include <Engine/Asset/FzTerrainImporter.hpp>
#include <Engine/Asset/FzTerrainSerializer.hpp>
#include <Engine/Asset/ImageImporter.hpp>
#include <Engine/Asset/MatAssetImporter.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/ModelImporter.hpp>
#include <Engine/Asset/TerrainAsset.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <limits>

namespace fbzz::asset {

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
std::unordered_map<std::string, renderer::ResourceHandle<renderer::TextureTag>>
    AssetManager::s_textures;
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
    RegisterImporter<ModelAsset>              (std::make_unique<FzModelImporter>());
    RegisterImporter<AnimationClip>           (std::make_unique<FzAnimImporter>());
    RegisterImporter<MaterialAsset>           (std::make_unique<MatAssetImporter>());
    RegisterImporter<AnimatorControllerAsset> (std::make_unique<AnimCtrlImporter>());
    RegisterImporter<TerrainAsset>            (std::make_unique<FzTerrainImporter>());
    RegisterImporter<TextureAsset>            (std::make_unique<ImageImporter>());
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
    s_textures.clear();

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
    for (auto it = s_textures.begin(); it != s_textures.end(); )
        it = it->second.IsValid() ? ++it : s_textures.erase(it);
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
    if (key.ends_with(".asset")) {
        model = FzAssetLoader::Load(fullPath, *s_resources);
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

// ── 旧 API: LoadTexture ──────────────────────────────────────────────────

renderer::ResourceHandle<renderer::TextureTag>
AssetManager::LoadTexture(const std::string& relativePath)
{
    assert(s_initialized);
    const std::string key = Normalize(relativePath);
    auto it = s_textures.find(key);
    if (it != s_textures.end()) return it->second;

    const auto h = s_resources->LoadTexture(ResolvePath(key, s_basePath));
    if (!h.IsValid())
        FBZZ_LOG_ERROR("AssetManager: Texture load failed [%s]", key.c_str());
    s_textures[key] = h;
    return h;
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
