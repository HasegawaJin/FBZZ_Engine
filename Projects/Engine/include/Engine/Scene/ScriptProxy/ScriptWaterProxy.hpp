/// @file    ScriptWaterProxy.hpp
/// @brief   Script から自 GO の WaterComponent (水面高さ・波の倍率・波紋) を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// 浮力スクリプトや水面エフェクトから水面高さを CPU 側で照会する用途にも使う。
/// 波の形そのものは .mat が持つ。Script から変えられるのは個体ごとの倍率と ON/OFF まで。
#pragma once

#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptWaterProxy {
    Script* script = nullptr;

    /// ワールド XZ での水面の高さ (ワールド Y)。time は Application::GetTime() などを渡す。
    float GetSurfaceHeightWorld(float worldX, float worldZ, float time) const;
    /// 自 GO のローカル XZ での、水面の基準面からの高さ。
    float GetSurfaceHeightLocal(float localX, float localZ, float time) const;

    /// .mat の波の振幅に掛ける倍率。嵐の演出などで一時的に荒らすときに使う。
    void  SetWaveAmplitudeScale(float scale) const;
    float GetWaveAmplitudeScale() const;
    void  SetGerstnerEnabled(bool enabled) const;

    /// 水流の速度 [m/s] (ワールド XZ)。.mat の flowDirection × currentSpeed。
    math::Vector2 GetCurrent() const;

    /// ワールド座標へ波紋を置く。strength は [0,1]。
    void AddRipple(const math::Vector3& worldPos, float strength = 0.5f) const;
    /// 波紋に加えて水しぶきを上げる。intensity は [0,1]。
    void Splash(const math::Vector3& worldPos, float intensity = 0.5f) const;

    void SetBuoyancyEnabled(bool enabled) const;
    void SetEnabled(bool enabled) const;
    bool IsEnabled() const;
};

} // namespace fbzz::scene
