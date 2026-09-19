/// @file    ScriptMeshTrailProxy.hpp
/// @brief   Script から MeshTrailComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-06
#pragma once

#include <Math/Vector4.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptMeshTrailProxy {
    Script* script = nullptr;

    /// @brief Mesh / SkinnedMesh の形状残像を切り替える。
    /// @note ダッシュ中や攻撃中だけオブジェクト形状の残像を出す用途を Script から簡潔に書ける。
    void SetEnabled(bool enabled, bool clearWhenDisabled = true) const;

    /// @brief 現在保持している残像サンプルを破棄する。
    /// @note SkinnedMesh の過去 bone palette は RenderSystem 側で安全に解放する。
    void Clear() const;

    /// @brief 残像が残る秒数を設定する。
    void SetDuration(float seconds) const;

    /// @brief サンプル追加頻度と最小移動距離を設定する。
    void SetSampling(float sampleInterval, float minVertexDist) const;

    /// @brief 最大残像枚数を設定する。
    void SetMaxSamples(int maxSamples) const;

    /// @brief 最新サンプルと最古サンプルの色を設定する。
    void SetColor(const math::Vector4& start, const math::Vector4& end) const;

    /// @brief 残像描画の背面カリング有無を切り替える。
    void SetDoubleSided(bool doubleSided) const;

    /// @brief SkinnedModel の指定 submesh を残像描画から除外する。
    void AddExcludedMeshIndex(int meshIndex) const;

    /// @brief submesh 除外設定を解除する。
    void ClearExcludedMeshIndices() const;

    /// @note SetEnabled(false, false) の自然消滅待ちを判定するための状態取得。
    [[nodiscard]] bool  IsEnabled() const;
    [[nodiscard]] float GetDuration() const;
    /// @return 現在保持している残像サンプル数。0 なら描画するものが残っていない。
    [[nodiscard]] int   GetSampleCount() const;
};

} // namespace fbzz::scene
