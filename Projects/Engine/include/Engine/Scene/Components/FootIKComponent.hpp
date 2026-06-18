// FBZZ Engine
// FootIKComponent.hpp | fbzz::scene
// Humanoid の足接地だけを扱う専用 IK 補正設定
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct FootIKComponent {
    bool enabled = true;

    // AnimatorController の state / motion IK Weight を乗算する。
    // WHY: Walk だけ接地補正し、Idle / Jump / Land の完成モーションは FK のまま再生するため。
    bool useAnimatorIKWeight = true;

    float weight = 1.0f;
    float rayUpRatio = 0.5f;
    float rayDownRatio = 1.2f;
    float footSurfaceOffset = 0.05f;
    float correctionDeadZone = 0.025f;
    float maxCorrection = 0.12f;

    // ゼロなら足首回転は FK 維持。指定時だけ斜面法線に合わせる。
    // WHY: 足裏ローカル軸はリグごとに違うため、自動推定しない。
    math::Vector3 footNormalAxis = math::Vector3::ZERO;

    const char* GetTypeName() const { return "Foot IK"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("useAnimatorIKWeight", useAnimatorIKWeight);
        r.Field("weight", weight);
        r.Field("rayUpRatio", rayUpRatio);
        r.Field("rayDownRatio", rayDownRatio);
        r.Field("footSurfaceOffset", footSurfaceOffset);
        r.Field("correctionDeadZone", correctionDeadZone);
        r.Field("maxCorrection", maxCorrection);
        r.Field("footNormalAxis", footNormalAxis);
    }
};

} // namespace fbzz::scene
