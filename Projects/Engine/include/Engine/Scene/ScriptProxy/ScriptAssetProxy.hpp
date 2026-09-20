/// @file    ScriptAssetProxy.hpp
/// @brief   Script からアセットの非同期読み込み・先読みを要求する窓口。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once

#include <cstdint>
#include <string_view>

namespace fbzz::scene {

class Script;

/// @brief 要求は不透明な ID で持つ。Engine の利用権そのものは見せない。
/// @note 要求はこのスクリプトが所有し、スクリプトの破棄 (OnDestroy の後・Play 停止) でまとめて手放す。
/// @note ID は実行中だけの値。シリアライズしない (保存するならパスを保存する)。
/// @see Docs/design/asset-streaming.md «識別子・利用権・API 契約»
struct ScriptAssetProxy {
    Script* script = nullptr;
    ~ScriptAssetProxy();

    /// 無効な要求 ID。
    static constexpr std::uint64_t kInvalid = 0;

    /// @brief テクスチャ・モデル (.fbx / .fzasset)・マテリアル (.mat) を拡張子で見分けて要求する。
    /// @param prefetch true なら先読み扱い (予算超過中は後回しにされる)。
    /// @return 要求 ID。種類が分からない・受け付けられない (予算超過など) なら kInvalid。
    std::uint64_t Request(std::string_view path, bool prefetch = false) const;
    [[nodiscard]] bool IsReady(std::uint64_t request) const;
    /// @brief 失敗して、Retry しない限り完成しない。
    [[nodiscard]] bool IsFailed(std::uint64_t request) const;
    /// @brief 希望品質 (0 が最高、1 段ごとにテクスチャは mip・モデルは LOD を 1 段落とす)。
    void SetQuality(std::uint64_t request, int quality) const;
    bool Retry(std::uint64_t request) const;
    /// @brief 自分の要求だけを手放す。他の利用者が居れば読み込みは続く。
    void Release(std::uint64_t request) const;

    /// @brief 登録済みシーンが参照するアセットを先読みする。遷移はしない。
    /// @return ファイル登録でないシーン名なら false。
    bool PrefetchScene(std::string_view sceneName) const;
    /// @brief 先読みの進み具合 [0, 1]。1 なら LoadScene で同期ロードが起きない。先読みしていなければ 0。
    [[nodiscard]] float GetScenePrefetchProgress(std::string_view sceneName) const;
};

} // namespace fbzz::scene
