/// @file    AssetStreamChannel.hpp
/// @brief   AssetStore<T> と結んだ非同期経路の土台。型ごとの Decode と転送だけを派生が書く。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <memory>
#include <string>
#include <utility>

namespace fbzz::asset {

/// @brief 完成物 1 つを持つだけの候補。CPU だけの型はそのまま使える。
template<typename T>
struct AssetCandidate : IAssetCandidate {
    std::unique_ptr<T> asset;
};

/// @brief AssetStore<T> を使うストア操作の共通実装。
/// @warning AssetStore<T> は DLL / EXE ごとに別の実体になる。チャンネルは «そのストアを使うモジュール» で
///          生成すること (エンジン型は FBZZEngine.dll 内、テスト専用型はテスト側)。
template<typename T>
class AssetStreamChannel : public IAssetStreamChannel {
public:
    [[nodiscard]] std::string_view TypeName() const override { return AssetStreamTypeName<T>(); }

    [[nodiscard]] std::string MakeKey(const std::string& reference) override
    {
        return AssetManager::CanonicalKey(reference);
    }

    [[nodiscard]] RawAssetHandle Lookup(const std::string& key) override
    {
        const auto& cache = AssetStore<T>::Get().cache;
        const auto it = cache.find(key);
        if (it == cache.end()) return {};
        return { it->second.id, it->second.gen };
    }

    [[nodiscard]] RawAssetHandle Reserve(const std::string& key) override
    {
        AssetStore<T>& store = AssetStore<T>::Get();
        const AssetHandle<T> handle = store.Reserve();
        store.cache[key] = handle;
        return { handle.id, handle.gen };
    }

    [[nodiscard]] bool IsLive(RawAssetHandle handle) const override
    {
        return AssetStore<T>::Get().IsLive(Typed(handle));
    }

    [[nodiscard]] bool IsFilled(RawAssetHandle handle) const override
    {
        return AssetStore<T>::Get().GetPtr(Typed(handle)) != nullptr;
    }

    [[nodiscard]] void* Get(RawAssetHandle handle) const override
    {
        return AssetStore<T>::Get().GetPtr(Typed(handle));
    }

    void Free(RawAssetHandle handle, const std::string& key) override
    {
        AssetStore<T>& store = AssetStore<T>::Get();
        const auto it = store.cache.find(key);
        if (it != store.cache.end() && it->second == Typed(handle)) store.cache.erase(it);
        store.Free(Typed(handle));
    }

    /// @note 同期 Load<T> が固定したスロットは外さない (明示的な Unload まで常駐する契約)。
    [[nodiscard]] bool Evict(RawAssetHandle handle, const std::string& key) override
    {
        if (AssetStore<T>::Get().IsPinned(Typed(handle))) return false;
        if (!ReleaseResident(handle)) return false;
        Free(handle, key);
        return true;
    }

    [[nodiscard]] bool LoadImmediately(RawAssetHandle handle, const std::string& key) override
    {
        return AssetManager::FillPendingWithoutPin<T>(key, Typed(handle));
    }

    [[nodiscard]] bool Publish(RawAssetHandle handle, IAssetCandidate& candidate) override
    {
        auto& typed = static_cast<AssetCandidate<T>&>(candidate);
        if (!typed.asset) return false;
        return AssetStore<T>::Get().Replace(Typed(handle), std::move(typed.asset));
    }

    void Discard(IAssetCandidate& /*candidate*/, bool /*deviceStillValid*/) override {}

protected:
    /// @brief スロットを返す前に、実体が握る外部資源 (GPU 実体など) を手放す。
    /// @return 他で使われていて手放せないなら false (スロットも返さない)。
    [[nodiscard]] virtual bool ReleaseResident(RawAssetHandle /*handle*/) { return true; }

    [[nodiscard]] static AssetHandle<T> Typed(RawAssetHandle handle) { return { handle.id, handle.gen }; }
    [[nodiscard]] static T* TypedGet(RawAssetHandle handle) { return AssetStore<T>::Get().GetPtr(Typed(handle)); }
};

} // namespace fbzz::asset
