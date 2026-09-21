/// @file    AudioSpatialComponents.hpp
/// @brief   Reverb Zone、遮蔽、Mixer Sendの空間Audio制御Component。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

struct AudioReverbZoneComponent {
    bool enabled = true;
    float innerRadius = 2.0f;
    float outerRadius = 10.0f;
    float wetLevel = 0.5f;
    float decayTime = 1.5f;
    float highFrequencyRatio = 0.7f;

    const char* GetTypeName() const { return "Audio Reverb Zone"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("innerRadius", innerRadius);
        r.Field("outerRadius", outerRadius);
        r.FloatRange("wetLevel", wetLevel, 0.0f, 1.0f);
        r.Field("decayTime", decayTime);
        r.FloatRange("highFrequencyRatio", highFrequencyRatio, 0.0f, 1.0f);
    }
};

struct AudioOcclusionComponent {
    bool enabled = true;
    int obstacleLayerMask = -1;
    float volumeAttenuation = 0.45f;
    float lowPass = 0.35f;
    /// @note レイキャストの間隔 (秒)。毎フレーム撃つ必要はない。
    float updateInterval = 0.1f;
    /// @note 遮蔽の有無が切り替わったあと、実際の効きがそこへ届くまでの秒数。
    /// @note レイキャストの結果は 0/1 の二値なので、そのまま当てると物陰を横切るたびに音が跳ぶ。
    float transitionTime = 0.15f;
    float currentOcclusion = 0.0f;
    /// @note レイキャストが最後に返した二値。currentOcclusion はここへ向かって動く。
    float m_targetOcclusion = 0.0f;
    float updateTimer = 0.0f;

    const char* GetTypeName() const { return "Audio Occlusion"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("obstacleLayerMask", obstacleLayerMask);
        r.FloatRange("volumeAttenuation", volumeAttenuation, 0.0f, 1.0f);
        r.FloatRange("lowPass", lowPass, 0.0f, 1.0f);
        r.Field("updateInterval", updateInterval);
        r.BeginField("transitionTime", "transitionTime");
        r.FloatRange("transitionTime", transitionTime, 0.0f, 1.0f);
        r.Tooltip("遮蔽の切り替わりを馴染ませる秒数 (0 で即時)");
        r.EndField();
        r.Readonly("currentOcclusion", currentOcclusion);
    }
};

struct AudioMixerSendComponent {
    bool enabled = true;
    /// @note 主出力とは別の補助バス。未知の名前は送信しない。
    std::string busName = "Master";
    /// @note 音源の音量・距離減衰・フェード適用後の信号に掛ける。主出力の音量は変えない。
    float sendLevel = 1.0f;

    const char* GetTypeName() const { return "Audio Mixer Send"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.BeginField("busName", "busName");
        r.SetFieldHint(IReflector::FieldHint::AudioBus);
        r.Field("busName", busName);
        r.EndField();
        r.FloatRange("sendLevel", sendLevel, 0.0f, 1.0f);
    }
};

} /// @note namespace fbzz::scene
