/// @file    VFXAudioEnvelope.hpp
/// @brief   VFXElement の生存窓に沿って AudioSource の音量を動かす。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note VFXElement は weight を VFXScreenEffect/CameraShake/TimeScale の 3 つにしか配れず、
/// @note 音は窓が閉じた瞬間に止まっていた。ループ音の立ち上がり・消え際を持たせるために追加。
/// @note AudioSourceComponent は BGM 等 VFX の生存窓を持たない常設音源も扱うため、エンベロープは
/// @note 時間の駆動元 (VFX) 側に置く。
#pragma once
#include <Engine/Scene/Components/VFX/VFXElement.hpp>
#include <Engine/Scene/VFX/ParticleCurve.hpp>
#include <Engine/Reflection/Reflection.hpp>

namespace fbzz::scene {

struct VFXAudioEnvelope {
    bool enabled = true; ///< 音源の音量と、設定されていればピッチへ生存窓のエンベロープを掛ける。

    /// @note 窓の進捗 0..1 に対する音量倍率。基準は AudioSourceComponent::volume。
    /// @note 既定は「立ち上がり 10% → 保持 → 消え際 30%」。
    ParticleCurve volumeCurve{
        {{ {0.0f, 0.0f}, {0.1f, 1.0f}, {0.7f, 1.0f}, {1.0f, 0.0f},
           {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f} }},
        4, ParticleCurveInterpolation::Linear }; ///< 生存窓の進捗から音源の基準音量に掛ける倍率を求めるカーブ。

    bool usePitchCurve = false; ///< ピッチも動かす。チャージ音が上がっていく表現に使う。
    ParticleCurve pitchCurve{
        {{ {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f},
           {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f} }},
        2, ParticleCurveInterpolation::Linear }; ///< 生存窓の進捗から音源の基準ピッチに掛ける倍率を求めるカーブ。

    /// @name ランタイム
    /// @{
    /// @note 捕獲した基準値。VFXLightEnvelope と同じ理由で、掴み直すと
    /// @note «下げた後の音量» を新しい基準にしてしまい、鳴らすたびに小さくなる。
    struct Base {
        float volume = 1.0f; ///< 音量カーブの適用前に保存した音源の基準音量。
        float pitch  = 1.0f; ///< ピッチカーブの適用前に保存した音源の基準ピッチ。
    };
    VFXCaptured<Base> base; ///< 初回の音源設定を保持して、エンベロープの積み重ねを防ぐ。

    const char* GetTypeName() const { return "VFX Audio Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("volumeCurve", volumeCurve);
        r.Field("usePitchCurve", usePitchCurve);
        r.Field("pitchCurve", pitchCurve);
    }
    /// @}
};

}
