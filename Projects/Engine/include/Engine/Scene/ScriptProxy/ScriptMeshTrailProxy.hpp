/// @file    ScriptMeshTrailProxy.hpp
/// @brief   Script から MeshTrailComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-06
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector4.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptMeshTrailProxy {
    Script* script = nullptr;

    /// @brief Mesh / SkinnedMesh の形状残像を切り替える。
    /// @note ダッシュ中や攻撃中だけオブジェクト形状の残像を出す用途を Script から簡潔に書ける。
    FBZZ_ENGINE_API void SetEnabled(bool enabled, bool clearWhenDisabled = true) const;

    /// @brief 現在保持している残像サンプルを破棄する。
    /// @note SkinnedMesh の過去 bone palette は RenderSystem 側で安全に解放する。
    FBZZ_ENGINE_API void Clear() const;

    /// @brief 残像が残る秒数を設定する。
    FBZZ_ENGINE_API void SetDuration(float seconds) const;

    /// @brief サンプル追加頻度と最小移動距離を設定する。
    FBZZ_ENGINE_API void SetSampling(float sampleInterval, float minVertexDist) const;

    /// @brief 最大残像枚数を設定する。
    FBZZ_ENGINE_API void SetMaxSamples(int maxSamples) const;

    /// @brief 最新サンプルと最古サンプルの色を設定する。
    FBZZ_ENGINE_API void SetColor(const math::Vector4& start, const math::Vector4& end) const;

    /// @brief 残像描画の背面カリング有無を切り替える。
    FBZZ_ENGINE_API void SetDoubleSided(bool doubleSided) const;

    /// @brief SkinnedModel の指定 submesh を残像描画から除外する。
    FBZZ_ENGINE_API void AddExcludedMeshIndex(int meshIndex) const;

    /// @brief submesh 除外設定を解除する。
    FBZZ_ENGINE_API void ClearExcludedMeshIndices() const;

    /// @note SetEnabled(false, false) の自然消滅待ちを判定するための状態取得。
    [[nodiscard]] FBZZ_ENGINE_API bool  IsEnabled() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetDuration() const;
    /// @return 現在保持している残像サンプル数。0 なら描画するものが残っていない。
    [[nodiscard]] FBZZ_ENGINE_API int   GetSampleCount() const;
};

} /// @note namespace fbzz::scene
