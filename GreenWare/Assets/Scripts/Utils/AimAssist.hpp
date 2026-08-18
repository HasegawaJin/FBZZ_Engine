// FBZZ Engine
// AimAssist.hpp | sandbox
// ソフトエイムの候補評価。アタッチしない純関数群。
//
// WHY 純関数に切り出すか:
//   企画書 6 章は「痛くあるべきなのはエイムミスではなく判断ミス」と書いている。つまり
//   ここの重み付けは体験の質に直結し、必ず何度も触ることになる。副作用の無い関数に
//   しておけば、数値だけを見て調整でき、周りのスクリプトを読み直さずに済む。
//
// WHY 画面座標へ投影しないか:
//   ScriptCameraProxy::WorldToScreenPoint は自分の GameObject の CameraComponent しか
//   見ないため、プレイヤー側のスクリプトからは呼べない。そもそも「画面中央からの距離」は
//   視線ベクトルとの成す角と同義なので、角度で評価すれば投影は要らない。
//   カメラの FOV が変わっても補正が要らないという副次的な利点もある。
#pragma once

#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <cmath>

namespace sandbox::aimassist {

using fbzz::math::Vector3;

// 視線と「カメラから候補へ向かうベクトル」の成す角 (度)。
// 候補がカメラとほぼ同位置にある場合は 0 を返す (自分を撃つことはないので実害はない)。
[[nodiscard]] inline float AngleToTargetDegrees(const Vector3& cameraPos,
                                                const Vector3& cameraForward,
                                                const Vector3& targetPos)
{
    const Vector3 toTarget = targetPos - cameraPos;
    if (toTarget.LengthSq() < fbzz::math::EPSILON) return 0.0f;

    const float cosine = fbzz::math::Clamp(
        Vector3::Dot(toTarget.Normalized(), cameraForward.Normalized()), -1.0f, 1.0f);
    return fbzz::math::ToDeg(std::acos(cosine));
}

// 候補の「狙いやすさ」。値が小さいほど良い候補。
//
// 角度を主・距離を従にしているのは、企画書のエイム補助の目的が
// 「小さいスライムに当たらなかった」を無くすことだから。画面のどこを向いているかが
// 第一で、距離は同じくらい正面にいる 2 体の決着にだけ使う。
//
// distanceWeight を上げると近い敵を優先し、下げると「今向いている方向」を強く尊重する。
[[nodiscard]] inline float TargetScore(float angleDegrees,
                                       float distance,
                                       float maxRange,
                                       float distanceWeight)
{
    const float normalizedDistance =
        maxRange > 0.0f ? fbzz::math::Clamp01(distance / maxRange) : 0.0f;
    return angleDegrees + normalizedDistance * distanceWeight;
}

// コーン内に入っているか。ここを外れた候補はスコアを計算するまでもなく捨てる。
[[nodiscard]] inline bool IsWithinCone(float angleDegrees, float coneDegrees)
{
    return angleDegrees <= coneDegrees;
}

} // namespace sandbox::aimassist
