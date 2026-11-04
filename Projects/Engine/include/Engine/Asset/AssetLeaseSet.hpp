/// @file    AssetLeaseSet.hpp
/// @brief   型の違う利用権をまとめて持ち、まとめて手放す束 (シーン単位の先読みに使う)。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

/// @brief 利用権の束。破棄・Release で自分の利用権だけを解く (他の利用者の要求は残る)。
/// @note move-only。メインスレッド専用。
/// @see Docs/design/asset-streaming.md «シーン切り替え・停止・復旧»
class AssetLeaseSet {
public:
    struct Progress {
        std::uint32_t total = 0;
        std::uint32_t ready = 0;
        std::uint32_t failed = 0;
        /// @note 失敗も «待つ必要が無い» に数える。
        [[nodiscard]] bool IsSettled() const { return ready + failed >= total; }
    };

    AssetLeaseSet() = default;
    explicit AssetLeaseSet(AssetStreamer& streamer) : m_streamer(&streamer) {}
    ~AssetLeaseSet() { Release(); }
    AssetLeaseSet(const AssetLeaseSet&) = delete;
    AssetLeaseSet& operator=(const AssetLeaseSet&) = delete;
    FBZZ_ENGINE_API AssetLeaseSet(AssetLeaseSet&& other) noexcept;
    FBZZ_ENGINE_API AssetLeaseSet& operator=(AssetLeaseSet&& other) noexcept;

    /// @brief 1 件要求して束へ加える。
    /// @return 受け付けられなかった理由。None なら加えた。
    FBZZ_ENGINE_API AssetLoadError Add(std::string_view typeName, const std::string& reference, const AssetRequestOptions& options);

    /// @brief 束の全員の優先度を変える (先読みから可視へ上げる等)。
    FBZZ_ENGINE_API void SetPriority(AssetPriority priority);
    /// @brief まだ完成していないものをこの場で同期ロードする。待機を許した箇所 (ロード画面) だけが使う。
    /// @return 同期ロードした件数。
    FBZZ_ENGINE_API std::uint32_t CompleteAllNow();

    [[nodiscard]] FBZZ_ENGINE_API Progress GetProgress() const;
    [[nodiscard]] std::size_t Size() const { return m_leases.size(); }
    FBZZ_ENGINE_API void Release();

private:
    struct Held {
        std::uint64_t  leaseId = 0;
        std::string    typeName;
        RawAssetHandle handle;
    };
    AssetStreamer*    m_streamer = nullptr;
    std::vector<Held> m_leases;
};

} /// @note namespace fbzz::asset
