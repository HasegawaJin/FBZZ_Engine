/// @file    ScriptWaterProxy.hpp
/// @brief   Script から WaterComponent の Gerstner 波パラメータを操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// 浮力スクリプトや水面エフェクトから水面高さを CPU 側で照会する用途にも使う。
#pragma once

#include <Math/Vector2.hpp>

namespace fbzz::scene {

class Script;

struct ScriptWaterProxy {
    Script* script = nullptr;

    // 自 GO の WaterComponent の Gerstner 波高さをワールド XZ 座標で計算する。
    // time は Application::GetTime() などを渡す。
    // WHY: WaterComponent::GetSurfaceHeightAt はローカル座標を要求するため、
    //      ここで worldPos → ローカル変換を行い Script 側の定型コードを排除する。
    float GetSurfaceHeightWorld(float worldX, float worldZ, float time) const;
    // ローカル座標版（自 GO が Water GO と同一の場合や変換済み座標を渡す場合に使う）。
    float GetSurfaceHeightLocal(float localX, float localZ, float time) const;

    // index は [0, 3]。範囲外は無視する。
    void SetWaveAmplitude (int index, float amplitude)   const;
    void SetWaveWavelength(int index, float wavelength)  const;
    void SetWaveSteepness (int index, float steepness)   const;
    void SetWaveDirection (int index, math::Vector2 dir) const;

    void SetGerstnerEnabled(bool enabled) const;
    void SetEnabled(bool enabled) const;
    bool IsEnabled() const;
};

} // namespace fbzz::scene
