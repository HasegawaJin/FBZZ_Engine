/// @file    VFXAudioEnvelope.hpp
/// @brief   VFXElement の生存窓に沿って AudioSource の音量を動かす。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 必要か:
///   VFXElement が weight を配る先は VFXScreenEffect / VFXCameraShake / VFXTimeScale の
///   3 つしか無く、音は «窓が閉じた瞬間に止まる» しかなかった。ワンショットなら
///   それでよいが、ループ音 (チャージ・ビームの持続音) は必ずぶつ切りになる。
///   立ち上がりと消え際を持てないと、鳴り始めと鳴り終わりだけが浮く。
///
/// WHY AudioSource に curve を足さないか:
///   AudioSourceComponent はシーンに常設する BGM・環境音でも使う型で、
///   そちらは «VFX の生存窓» を持たない。時間の駆動元を持つ側 (VFX) に
///   エンベロープを置く方が、駆動されない場面での意味が明確になる。
#pragma once
#include <Engine/Scene/Components/VFXElement.hpp>  // VFXCaptured
#include <Engine/Scene/ParticleCurve.hpp>
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

struct VFXAudioEnvelope {
    bool enabled = true;

    /// 窓の進捗 0..1 に対する音量倍率。基準は AudioSourceComponent::volume。
    /// 既定は「立ち上がり 10% → 保持 → 消え際 30%」。
    ParticleCurve volumeCurve{
        {{ {0.0f, 0.0f}, {0.1f, 1.0f}, {0.7f, 1.0f}, {1.0f, 0.0f},
           {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f} }},
        4, ParticleCurveInterpolation::Linear };

    /// ピッチも動かす。チャージ音が上がっていく表現に使う。
    bool usePitchCurve = false;
    ParticleCurve pitchCurve{
        {{ {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f},
           {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f} }},
        2, ParticleCurveInterpolation::Linear };

    // --- ランタイム ---
    // 捕獲した基準値。VFXLightEnvelope と同じ理由で、掴み直すと
    // «下げた後の音量» を新しい基準にしてしまい、鳴らすたびに小さくなる。
    struct Base {
        float volume = 1.0f;
        float pitch  = 1.0f;
    };
    VFXCaptured<Base> base;

    const char* GetTypeName() const { return "VFX Audio Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("volumeCurve", volumeCurve);
        r.Field("usePitchCurve", usePitchCurve);
        r.Field("pitchCurve", pitchCurve);
    }
};

} // namespace fbzz::scene
