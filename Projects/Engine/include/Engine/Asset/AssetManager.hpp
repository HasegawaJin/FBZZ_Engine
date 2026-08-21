// FBZZ Engine
// AssetManager.hpp | fbzz::asset
// 統一アセットロード API
//
// 新 API: AssetManager::Load<T>(path) → AssetHandle<T>
//   対応型: ModelAsset, AnimationClip, TextureAsset, MaterialAsset,
//           TerrainAsset, AnimatorControllerAsset
//   RegisterImporter<T>() で IAssetImporter<T> を登録してから使う (Init で実施)
//
// 旧 API (後方互換 — 移行中のコールサイト向け):
//   LoadModel(path)   → Model*
//   LoadMaterial(path)→ ResourceHandle<MaterialAssetTag>
//   GetMaterial(h)    → MaterialAsset*
//   テクスチャは ResourceManager::LoadTexture() を使うこと。
//
// 旧 API への Load<Model> シンタックスは LoadModel() に移行すること。
#pragma once
#include <Engine/Asset/AssetHandle.hpp>
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::asset {

struct AnimationClip;
struct AnimatorControllerAsset;
struct ModelAsset;
struct PhysicsMaterialAsset;
struct TerrainAsset;
struct TextureAsset;

// ── 型ごとスロットプール (Meyers シングルトン) ─────────────────────────────
// ヘッダーに置いて Load<T> を完全インライン化。type_index マップを使わない。
template<typename T>
class AssetStore {
public:
    struct Slot {
        std::unique_ptr<T> asset;
        uint32_t gen      = 1;
        bool     occupied = false;
    };

    static AssetStore& Get() { static AssetStore s; return s; }

    // id=0 を Null として予約するため、内部インデックスに +1 のオフセットを掛ける
    AssetHandle<T> Alloc(std::unique_ptr<T> asset_) {
        uint32_t idx = 0;
        if (!freeList.empty()) {
            idx = freeList.back();
            freeList.pop_back();
        } else {
            idx = static_cast<uint32_t>(slots.size());
            slots.emplace_back();
        }
        Slot& s   = slots[idx];
        s.asset   = std::move(asset_);
        s.occupied = true;
        return AssetHandle<T>{ idx + 1u, s.gen };
    }

    bool IsLive(AssetHandle<T> h) const {
        if (!h.IsValid()) return false;
        const uint32_t idx = h.id - 1u;
        if (idx >= slots.size()) return false;
        return slots[idx].occupied && slots[idx].gen == h.gen;
    }

    T* GetPtr(AssetHandle<T> h) {
        if (!IsLive(h)) return nullptr;
        return slots[h.id - 1u].asset.get();
    }

    // ハンドルを保ったままスロットの中身だけを差し替える。
    // WHY gen を進めないか: 差し替えの目的は「同じアセットの新しい版」を配ることなので、
    //     既に配ったハンドルは生きたままでなければならない。gen を進めると、Scene /
    //     Component が持つハンドルが一斉に死んで参照切れとして現れる。
    bool Replace(AssetHandle<T> h, std::unique_ptr<T> asset_) {
        if (!IsLive(h) || !asset_) return false;
        slots[h.id - 1u].asset = std::move(asset_);
        return true;
    }

    void Free(AssetHandle<T> h) {
        if (!IsLive(h)) return;
        const uint32_t idx = h.id - 1u;
        Slot& s = slots[idx];
        s.asset.reset();
        s.occupied = false;
        s.gen = (s.gen == (std::numeric_limits<uint32_t>::max)()) ? 1u : s.gen + 1u;
        freeList.push_back(idx);
    }

    void Clear() {
        slots.clear();
        freeList.clear();
        cache.clear();
        importer.reset();
    }

    std::vector<Slot>                               slots;
    std::vector<uint32_t>                           freeList;
    std::unordered_map<std::string, AssetHandle<T>> cache;
    std::unique_ptr<IAssetImporter<T>>              importer;

private:
    AssetStore() = default;
};

// ── AssetManager ────────────────────────────────────────────────────────────

class AssetManager {
public:
    static void Init(renderer::ResourceManager& resources, const std::string& basePath = "assets/");
    static void UnloadAll();
    static void FlushFailed();
    static int  GetFlushGeneration();

    /// 指定ファイルを参照しているキャッシュ済みアセットを、ハンドルを保ったまま再取り込みする。
    /// @param absPath 監視イベントが返す絶対パス
    /// @return 差し替えた件数。0 ならこのファイルはどのストアにも載っていない
    ///
    /// 取り込みに失敗した場合は既存の中身を残す。書き込み途中のファイルを掴んで
    /// 動いていたアセットを壊さないため。
    static int ReloadPath(const std::string& absPath);

    /// アセットの中身が変わるたびに進む世代番号。
    /// クリップのコピーやマスクのように、ストアの中身から派生キャッシュを作る側が
    /// 「作り直すべきか」を 1 つの整数比較で判断するために使う。
    static int GetAssetGeneration();

    /// ストアを通さず直読みしているアセット (.mask 等) を差し替えたときに、
    /// 派生キャッシュへ「作り直せ」と伝えるための明示的な世代更新。
    static void BumpAssetGeneration();

    [[nodiscard]] static std::string ResolveAssetPath(const std::string& path);

    // 原本 FBX に対応する Library/Baked/<fbx-guid>/ の絶対パスを返す (末尾に '/' なし)。
    // guid が引けない (Assets 外の FBX 等) 場合は空文字。
    //
    // WHY 公開するか: AssetBrowser がサブアセット (.anim 等) を列挙するには、
    //     ファイル名を知らない状態で「隔離先のディレクトリ」を知る必要がある。
    //     ResolveAssetPath は実在するファイルのパスしか返せないため、
    //     ディレクトリを引く経路を別に用意する。
    [[nodiscard]] static std::string BakedDirForSource(const std::string& sourceAbsPath);

    // ── 新統一 API ─────────────────────────────────────────────────────

    template<typename T>
    static void RegisterImporter(std::unique_ptr<IAssetImporter<T>> imp) {
        AssetStore<T>::Get().importer = std::move(imp);
    }

    template<typename T>
    static AssetHandle<T> Load(const std::string& relativePath) {
        assert(S_init() && "AssetManager::Init() must be called first");
        const std::string key = Normalize(relativePath);
        AssetStore<T>& store = AssetStore<T>::Get();

        const auto it = store.cache.find(key);
        if (it != store.cache.end()) return it->second;

        std::unique_ptr<T> asset;
        if (store.importer)
            asset = store.importer->Import(ResolvePath(key, S_base()), S_res());

        if (!asset) {
            store.cache[key] = AssetHandle<T>::Null();
            return AssetHandle<T>::Null();
        }
        const AssetHandle<T> h = store.Alloc(std::move(asset));
        store.cache[key] = h;
        return h;
    }

    template<typename T>
    static T* Get(AssetHandle<T> h) {
        return AssetStore<T>::Get().GetPtr(h);
    }

    template<typename T>
    static void Unload(const std::string& relativePath) {
        const std::string key = Normalize(relativePath);
        AssetStore<T>& store = AssetStore<T>::Get();
        const auto it = store.cache.find(key);
        if (it == store.cache.end()) return;
        store.Free(it->second);
        store.cache.erase(it);
    }

    // ── 旧 API (後方互換) ──────────────────────────────────────────────

    // .fbx / .fzasset → Model* (呼び出し元は LoadModel に移行すること)
    static Model* LoadModel(const std::string& relativePath);

    static renderer::ResourceHandle<renderer::MaterialAssetTag>
        LoadMaterial(const std::string& relativePath);
    static MaterialAsset* GetMaterial(renderer::ResourceHandle<renderer::MaterialAssetTag> h);
    static void           UnloadMaterial(const std::string& relativePath);

private:
    static renderer::ResourceManager* s_resources;
    static std::string                s_basePath;
    static std::string                s_engineBasePath;
    static bool                       s_initialized;

    static std::unordered_map<std::string, std::unique_ptr<Model>>    s_models;

    struct MatSlot {
        std::unique_ptr<MaterialAsset> asset;
        uint32_t gen      = 1;
        bool     occupied = false;
    };
    static std::vector<MatSlot>   s_materialSlots;
    static std::vector<uint32_t>  s_materialFreeList;
    static std::unordered_map<std::string, renderer::ResourceHandle<renderer::MaterialAssetTag>> s_materials;

    static std::string Normalize(const std::string& path);
    static std::string ResolvePath(const std::string& key, const std::string& basePath);
    // DLL-boundary helpers: static data members cannot cross DLL boundaries directly,
    // so the header-inline Load<T> template must call these exported functions instead.
    static bool                        S_init() noexcept;
    static const std::string&          S_base() noexcept;
    static renderer::ResourceManager*  S_res()  noexcept;
    template<typename T>
    static AssetHandle<T> LoadFromStore(const std::string& relativePath);
    template<typename T>
    static T* GetFromStore(AssetHandle<T> h);
    template<typename T>
    static void UnloadFromStore(const std::string& relativePath);
    template<typename T>
    static int ReloadFromStore(const std::string& absPath);
    static renderer::ResourceHandle<renderer::MaterialAssetTag>
        AllocMaterialSlot(std::unique_ptr<MaterialAsset>);
    static bool IsMaterialLive(renderer::ResourceHandle<renderer::MaterialAssetTag>);
};

template<>
AssetHandle<ModelAsset> AssetManager::Load<ModelAsset>(const std::string& relativePath);
template<>
AssetHandle<AnimationClip> AssetManager::Load<AnimationClip>(const std::string& relativePath);
template<>
AssetHandle<MaterialAsset> AssetManager::Load<MaterialAsset>(const std::string& relativePath);
template<>
AssetHandle<AnimatorControllerAsset> AssetManager::Load<AnimatorControllerAsset>(const std::string& relativePath);
template<>
AssetHandle<TerrainAsset> AssetManager::Load<TerrainAsset>(const std::string& relativePath);
template<>
AssetHandle<TextureAsset> AssetManager::Load<TextureAsset>(const std::string& relativePath);
template<>
AssetHandle<PhysicsMaterialAsset> AssetManager::Load<PhysicsMaterialAsset>(const std::string& relativePath);

template<>
ModelAsset* AssetManager::Get<ModelAsset>(AssetHandle<ModelAsset> h);
template<>
AnimationClip* AssetManager::Get<AnimationClip>(AssetHandle<AnimationClip> h);
template<>
MaterialAsset* AssetManager::Get<MaterialAsset>(AssetHandle<MaterialAsset> h);
template<>
AnimatorControllerAsset* AssetManager::Get<AnimatorControllerAsset>(AssetHandle<AnimatorControllerAsset> h);
template<>
TerrainAsset* AssetManager::Get<TerrainAsset>(AssetHandle<TerrainAsset> h);
template<>
TextureAsset* AssetManager::Get<TextureAsset>(AssetHandle<TextureAsset> h);
template<>
PhysicsMaterialAsset* AssetManager::Get<PhysicsMaterialAsset>(AssetHandle<PhysicsMaterialAsset> h);

template<>
void AssetManager::Unload<ModelAsset>(const std::string& relativePath);
template<>
void AssetManager::Unload<AnimationClip>(const std::string& relativePath);
template<>
void AssetManager::Unload<MaterialAsset>(const std::string& relativePath);
template<>
void AssetManager::Unload<AnimatorControllerAsset>(const std::string& relativePath);
template<>
void AssetManager::Unload<TerrainAsset>(const std::string& relativePath);
template<>
void AssetManager::Unload<TextureAsset>(const std::string& relativePath);
template<>
void AssetManager::Unload<PhysicsMaterialAsset>(const std::string& relativePath);

} // namespace fbzz::asset
