/// @file    AssetManager.hpp
/// @brief   統一アセットロード API。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// `AssetManager::Load<T>(path)` → `AssetHandle<T>`、`Get<T>(h)` → `T*`、`Unload<T>(path)`。
/// 対応型: Model, ModelAsset, AnimationClip, TextureAsset, MaterialAsset,
/// TerrainAsset, AnimatorControllerAsset, SequenceAsset
/// RegisterImporter<T>() で IAssetImporter<T> を登録してから使う (Init で実施)
///
/// @note テクスチャの GPU 実体は ResourceManager::LoadTexture() が持つ。
/// @see Docs/design/ — Model は .fzasset (ModelAsset) から組み立てる旧表現で、
///      メッシュ・マテリアル・クリップを一括保持する。新規コードは ModelAsset を使う。
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
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::asset {

struct AnimationClip;
struct AnimatorControllerAsset;
struct ModelAsset;
struct PhysicsMaterialAsset;
struct ClothAsset;
struct SequenceAsset;
struct TerrainAsset;
struct TextureAsset;

/// @brief 型ごとスロットプール (Meyers シングルトン)。
/// @note ヘッダーに置いて Load<T> を完全インライン化する。type_index マップは使わない。
template<typename T>
class AssetStore {
public:
    struct Slot {
        std::unique_ptr<T> asset;
        uint32_t gen      = 1;
        bool     occupied = false;
    };

    static AssetStore& Get() { static AssetStore s; return s; }

    /// @brief id=0 を Null として予約するため、内部インデックスへ +1 のオフセットを掛けて確保する。
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

    /// @brief ハンドルを保ったままスロットの中身だけを差し替える。
    /// @note gen は進めない。進めると Scene / Component が持つ既存ハンドルが一斉に死に、参照切れとして現れる。
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

    /// @brief ホットリロードで「その場で中身を差し替えられる」拡張子の一覧。
    /// @note ReloadPath が差し替える型と Editor 側の監視ゲートが共有する、唯一の正本リスト。
    /// @note ここに載るのは «キャッシュ済みの値を入れ替えるだけで済む» 型だけ。GPU 資源を持つ型 (テクスチャ・モデル) は別経路で、ここには載せない。
    [[nodiscard]] static std::span<const std::string_view> HotReloadableExtensions();

    /// @brief 拡張子 (先頭のドット込み・大小問わず) がホットリロード対象か。
    [[nodiscard]] static bool IsHotReloadableExtension(std::string_view extension);

    /// @brief アセットの中身が変わるたびに進む世代番号。
    /// @note 派生キャッシュを持つ側 (クリップのコピー・マスク等) が「作り直すべきか」を整数比較 1 つで判断するために使う。
    static int GetAssetGeneration();

    /// @brief ストアを通さず直読みしているアセット (.mask 等) を差し替えたときの、派生キャッシュへの明示的な世代更新。
    static void BumpAssetGeneration();

    [[nodiscard]] static std::string ResolveAssetPath(const std::string& path);

    /// @brief 原本 FBX に対応する `Library/Baked/<fbx-guid>/` の絶対パスを返す (末尾 '/' なし)。
    /// @return guid が引けない (Assets 外の FBX 等) なら空文字。
    /// @note AssetBrowser がファイル名を知らない状態でサブアセット (.anim 等) の隔離先ディレクトリを列挙するために使う。ResolveAssetPath は実在ファイルしか返せない。
    [[nodiscard]] static std::string BakedDirForSource(const std::string& sourceAbsPath);

    /// @name 新統一 API
    /// @{

    template<typename T>
    static void RegisterImporter(std::unique_ptr<IAssetImporter<T>> imp) {
        AssetStore<T>::Get().importer = std::move(imp);
    }

    template<typename T>
    static AssetHandle<T> Load(const std::string& relativePath) {
        assert(S_init() && "AssetManager::Init() must be called first");
        const std::string key = CacheKey(relativePath);
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

    /// Load<T>() してそのまま実体を引く。ハンドルを持ち回らない呼び出し元向け。
    /// @return ロードに失敗したら nullptr。
    /// @warning 返り値はストアの所有物。UnloadAll / Unload<T>() を跨いで保持しないこと。
    template<typename T>
    static T* LoadAndGet(const std::string& relativePath) {
        return Get<T>(Load<T>(relativePath));
    }

    template<typename T>
    static void Unload(const std::string& relativePath) {
        const std::string key = CacheKey(relativePath);
        AssetStore<T>& store = AssetStore<T>::Get();
        const auto it = store.cache.find(key);
        if (it == store.cache.end()) return;
        store.Free(it->second);
        store.cache.erase(it);
    }

    /// @}

private:
    static renderer::ResourceManager* s_resources;
    static std::string                s_basePath;
    static std::string                s_engineBasePath;
    static bool                       s_initialized;

    static std::string Normalize(const std::string& path);

    /// @brief キャッシュキーを 1 つに寄せる。解決できる guid 参照は `"Assets/..."` 相対へ畳む。
    /// @note シーンは `"guid:<hex>|Assets/X.mat"`、Inspector/D&D は `"Assets/X.mat"` で同じファイルを引く。キーが分かれるとストアに実体が 2 つでき、Inspector の編集が描画側へ届かない。
    /// @note 解決できない参照は元のキーのまま通す。ResolvePath の «参照切れ» 報告を残すため。
    static std::string CacheKey(const std::string& path);
    static std::string ResolvePath(const std::string& key, const std::string& basePath);

    /// @name DLL 境界ヘルパー
    /// @note static データメンバーは DLL 境界を直接越えられないため、ヘッダーインラインの Load<T> 等はこれらのエクスポート関数を呼ぶ。
    /// @{
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
    /// @}
};

template<>
AssetHandle<Model> AssetManager::Load<Model>(const std::string& relativePath);
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
AssetHandle<ClothAsset> AssetManager::Load<ClothAsset>(const std::string& relativePath);
template<>
AssetHandle<SequenceAsset> AssetManager::Load<SequenceAsset>(const std::string& relativePath);

template<>
Model* AssetManager::Get<Model>(AssetHandle<Model> h);
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
ClothAsset* AssetManager::Get<ClothAsset>(AssetHandle<ClothAsset> h);
template<>
SequenceAsset* AssetManager::Get<SequenceAsset>(AssetHandle<SequenceAsset> h);

template<>
void AssetManager::Unload<Model>(const std::string& relativePath);
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
template<>
void AssetManager::Unload<ClothAsset>(const std::string& relativePath);
template<>
void AssetManager::Unload<SequenceAsset>(const std::string& relativePath);

} // namespace fbzz::asset
