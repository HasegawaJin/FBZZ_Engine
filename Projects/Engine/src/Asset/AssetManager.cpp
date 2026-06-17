// FBZZ Engine
// AssetManager.cpp | fbzz::asset
// Model と Texture のロードおよびキャッシュ管理
// 相対パスを正規化し、同じアセットを重複ロードしない。
// Texture は ResourceManager、Model は ModelImporter を通して生成する。
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FzAssetLoader.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelImporter.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <limits>

namespace fbzz::asset {

renderer::ResourceManager* AssetManager::s_resources = nullptr;
std::string AssetManager::s_basePath = "Assets/";
bool AssetManager::s_initialized = false;
std::unordered_map<std::string, std::unique_ptr<Model>>    AssetManager::s_models;
std::unordered_map<std::string, renderer::ResourceHandle<renderer::TextureTag>> AssetManager::s_textures;
static int s_flushGeneration = 0;

// MaterialAsset スロットプール (静的インスタンス)
// slot 0 は ResourceHandle デフォルト値 (id=0) との衝突防止のため null 予約。Init() で push する。
std::vector<AssetManager::MatSlot>   AssetManager::s_materialSlots;
std::vector<uint32_t>                AssetManager::s_materialFreeList;
std::unordered_map<std::string, renderer::ResourceHandle<renderer::MaterialAssetTag>> AssetManager::s_materials;

std::string AssetManager::Normalize(const std::string& path)
{
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

// basePath + key を優先して試し、存在しなければ key をそのまま使う
static bool StartsWithIgnoreCase(const std::string& s, const char* prefix)
{
    for (size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= s.size()) return false;
        const auto a = static_cast<unsigned char>(s[i]);
        const auto b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

static bool IsAbsolutePath(const std::string& path)
{
    if (path.empty()) return false;
    if (path[0] == '/') return true;
    return path.size() >= 3 &&
           std::isalpha(static_cast<unsigned char>(path[0])) &&
           path[1] == ':' &&
           path[2] == '/';
}

static std::string EnsureTrailingSlash(std::string path)
{
    if (!path.empty() && path.back() != '/')
        path.push_back('/');
    return path;
}

static std::string ResolvePath(const std::string& key, const std::string& basePath)
{
    if (IsAbsolutePath(key))
        return key;

    std::string assetKey = key;
    if (StartsWithIgnoreCase(assetKey, "Assets/"))
        assetKey = assetKey.substr(7);

    const std::string full = EnsureTrailingSlash(basePath) + assetKey;
    if (util::FileSystem::Exists(full)) return full;
    if (util::FileSystem::Exists(key))  return key;
    return full; // 存在しなくても呼び出し元に任せる (エラーログは呼び出し元で)
}

void AssetManager::Init(renderer::ResourceManager& resources, const std::string& basePath)
{
    assert(!s_initialized && "AssetManager::Init() must be called once");
    s_resources = &resources;
    s_basePath = Normalize(basePath);
    s_initialized = true;
    // slot 0 を null 予約: ResourceHandle::IsValid() が id != 0 で判定するため
    if (s_materialSlots.empty())
        s_materialSlots.emplace_back();
}

std::string AssetManager::ResolveAssetPath(const std::string& path)
{
    return ResolvePath(Normalize(path), s_basePath);
}

void AssetManager::UnloadAll()
{
    s_models.clear();
    s_materials.clear();
    s_materialSlots.clear();
    s_materialSlots.emplace_back(); // slot 0 を null として再予約
    s_materialFreeList.clear();
    s_textures.clear();
    s_resources = nullptr;
    s_initialized = false;
}

renderer::ResourceHandle<renderer::TextureTag> AssetManager::LoadTexture(const std::string& relativePath)
{
    assert(s_initialized && "AssetManager::Init() must be called first");

    const std::string key = Normalize(relativePath);
    auto it = s_textures.find(key);
    if (it != s_textures.end()) return it->second;

    const std::string fullPath = ResolvePath(key, s_basePath);

    const auto texture = s_resources->LoadTexture(fullPath);

    if (!texture.IsValid()) {
        FBZZ_LOG_ERROR("AssetManager: Texture load failed [%s]", fullPath.c_str());
        return {};
    }

    s_textures[key] = texture;
    return texture;
}

template<>
Model* AssetManager::Load<Model>(const std::string& relativePath)
{
    assert(s_initialized && "AssetManager::Init() must be called first");

    const std::string key = Normalize(relativePath);
    auto it = s_models.find(key);
    if (it != s_models.end()) return it->second.get();

    const std::string fullPath = ResolvePath(key, s_basePath);

    std::unique_ptr<Model> model;
    // .asset はネイティブバイナリローダーへ委譲する (Assimp 不要)
    if (key.ends_with(".asset")) {
        model = FzAssetLoader::Load(fullPath, *s_resources);
    } else {
        model = ModelImporter::Import(fullPath, *s_resources);
    }

    if (!model) {
        FBZZ_LOG_WARN("AssetManager: Model load failed [%s]", fullPath.c_str());
        // nullptr をキャッシュして毎フレームのリトライスパムを防ぐ。
        // FlushFailed() 呼び出しでクリアすれば再試行できる。
        s_models[key] = nullptr;
        return nullptr;
    }

    Model* ptr = model.get();
    s_models[key] = std::move(model);
    return ptr;
}

// --- MaterialAsset スロットプール ---

bool AssetManager::IsMaterialLive(renderer::ResourceHandle<renderer::MaterialAssetTag> h)
{
    if (!h.IsValid()) return false;
    if (h.id >= s_materialSlots.size()) return false;
    const MatSlot& slot = s_materialSlots[h.id];
    return slot.occupied && slot.gen == h.gen;
}

renderer::ResourceHandle<renderer::MaterialAssetTag> AssetManager::AllocMaterialSlot(std::unique_ptr<MaterialAsset> asset)
{
    uint32_t id = 0;
    if (!s_materialFreeList.empty()) {
        id = s_materialFreeList.back();
        s_materialFreeList.pop_back();
    } else {
        id = static_cast<uint32_t>(s_materialSlots.size());
        s_materialSlots.emplace_back();
    }
    MatSlot& slot = s_materialSlots[id];
    slot.asset    = std::move(asset);
    slot.occupied = true;
    return { id, slot.gen };
}

renderer::ResourceHandle<renderer::MaterialAssetTag> AssetManager::LoadMaterial(const std::string& relativePath)
{
    assert(s_initialized && "AssetManager::Init() must be called first");

    const std::string key = Normalize(relativePath);
    auto it = s_materials.find(key);
    if (it != s_materials.end()) return it->second; // null ハンドル (ロード失敗) もキャッシュ済み

    const std::string fullPath = ResolvePath(key, s_basePath);
    auto material = std::make_unique<MaterialAsset>();
    if (!LoadMaterialAssetFromFile(fullPath, *material)) {
        FBZZ_LOG_WARN("AssetManager: MaterialAsset load failed [%s]", fullPath.c_str());
        // WHY: 欠落 .mat はフレームごとに再試行されやすい。null ハンドルキャッシュでログスパムを防ぐ。
        s_materials[key] = renderer::ResourceHandle<renderer::MaterialAssetTag>::Null();
        return {};
    }

    const auto handle = AllocMaterialSlot(std::move(material));
    s_materials[key]  = handle;
    return handle;
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

    const auto handle = it->second;
    s_materials.erase(it);

    if (!IsMaterialLive(handle)) return;
    MatSlot& slot = s_materialSlots[handle.id];
    slot.asset.reset();
    slot.occupied = false;
    // gen を進めて古いハンドルを無効化する
    const uint32_t maxGen = std::numeric_limits<uint32_t>::max();
    slot.gen = (slot.gen == maxGen) ? 1u : slot.gen + 1u;
    s_materialFreeList.push_back(handle.id);
}

void AssetManager::FlushFailed()
{
    for (auto it = s_models.begin(); it != s_models.end(); ) {
        if (!it->second) it = s_models.erase(it);
        else             ++it;
    }
    for (auto it = s_materials.begin(); it != s_materials.end(); ) {
        if (!it->second.IsValid()) it = s_materials.erase(it);
        else                       ++it;
    }
    for (auto it = s_textures.begin(); it != s_textures.end(); ) {
        if (!it->second.IsValid()) it = s_textures.erase(it);
        else                       ++it;
    }
    ++s_flushGeneration;
}

int AssetManager::GetFlushGeneration() { return s_flushGeneration; }

template<>
void AssetManager::Unload<Model>(const std::string& relativePath)
{
    s_models.erase(Normalize(relativePath));
}

} // namespace fbzz::asset
