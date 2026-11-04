/// @file    ScriptWaterProxy.hpp
/// @brief   Script から自 GO の WaterComponent (水面高さ・波の倍率・波紋) を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note 浮力スクリプトや水面エフェクトから水面高さを CPU 側で照会する用途にも使う。
/// @note 波の形そのものは .mat が持つ。Script から変えられるのは個体ごとの倍率と ON/OFF まで。
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptWaterProxy {
    Script* script = nullptr;

    /// @note ワールド XZ での水面の高さ (ワールド Y)。time は Application::GetTime() などを渡す。
    FBZZ_ENGINE_API float GetSurfaceHeightWorld(float worldX, float worldZ, float time) const;
    /// @note 自 GO のローカル XZ での、水面の基準面からの高さ。
    FBZZ_ENGINE_API float GetSurfaceHeightLocal(float localX, float localZ, float time) const;

    /// @note .mat の波の振幅に掛ける倍率。嵐の演出などで一時的に荒らすときに使う。
    FBZZ_ENGINE_API void  SetWaveAmplitudeScale(float scale) const;
    FBZZ_ENGINE_API float GetWaveAmplitudeScale() const;
    FBZZ_ENGINE_API void  SetGerstnerEnabled(bool enabled) const;

    /// @note 水流の速度 [m/s] (ワールド XZ)。.mat の flowDirection × currentSpeed。
    FBZZ_ENGINE_API math::Vector2 GetCurrent() const;

    /// @note ワールド座標へ波紋を置く。strength は [0,1]。
    FBZZ_ENGINE_API void AddRipple(const math::Vector3& worldPos, float strength = 0.5f) const;
    /// @note 波紋に加えて水しぶきを上げる。intensity は [0,1]。
    FBZZ_ENGINE_API void Splash(const math::Vector3& worldPos, float intensity = 0.5f) const;

    FBZZ_ENGINE_API void SetBuoyancyEnabled(bool enabled) const;
    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API bool IsEnabled() const;
};

} /// @note namespace fbzz::scene
