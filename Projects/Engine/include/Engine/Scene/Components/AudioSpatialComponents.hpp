// FBZZ Engine
// AudioSpatialComponents.hpp | fbzz::scene
// Reverb Zone、遮蔽、Mixer Sendの空間Audio制御Component
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
    float updateInterval = 0.1f;
    float currentOcclusion = 0.0f;
    float updateTimer = 0.0f;

    const char* GetTypeName() const { return "Audio Occlusion"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("obstacleLayerMask", obstacleLayerMask);
        r.FloatRange("volumeAttenuation", volumeAttenuation, 0.0f, 1.0f);
        r.FloatRange("lowPass", lowPass, 0.0f, 1.0f);
        r.Field("updateInterval", updateInterval);
        r.Readonly("currentOcclusion", currentOcclusion);
    }
};

struct AudioMixerSendComponent {
    bool enabled = true;
    std::string busName = "Master";
    float sendLevel = 1.0f;
    bool preFader = false;

    const char* GetTypeName() const { return "Audio Mixer Send"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("busName", busName);
        r.FloatRange("sendLevel", sendLevel, 0.0f, 1.0f);
        r.Field("preFader", preFader);
    }
};

} // namespace fbzz::scene
