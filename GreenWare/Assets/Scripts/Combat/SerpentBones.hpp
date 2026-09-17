/// @file    SerpentBones.hpp
/// @brief   蛇の骨と部位の名前、鎖を経路へ沿わせるための回転。アタッチしないユーティリティ
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// @note 節の名前 (`Seg07`/`S07`/`HB_Seg07`) が骨・当たり・輪郭の 3 者で食い違うと
///       «その節だけ» 反応しないバグになる。Spine・HitboxRig・SerpentRig は必ず
///       ここの関数から名前を引く。
/// @note 添字は頭からの本数。骨の親子は尾が根 (Root → Seg28 → … → Head) だが、
///       経路も節の欠けも «頭から何本目» で効くため、向きを 2 つ持たない。
#pragma once

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace sandbox::serpent {

/// 変形する節の数。頭は含まない (boss-serpent.md「体」28 節 + 頭)。
inline constexpr int kSegmentCount = 28;
/// 骨の総数。0 = Head、1..28 = Seg01..Seg28。
inline constexpr int kBoneCount = kSegmentCount + 1;

/// 2 桁の 0 詰め。std::format は DLL 側のランタイムに依存させたくないので手で書く。
[[nodiscard]] inline std::string Pad2(int value)
{
    const int v = std::clamp(value, 0, 99);
    return std::string(1, static_cast<char>('0' + v / 10)) +
           std::string(1, static_cast<char>('0' + v % 10));
}

/// 頭からの添字に対応するボーン名。
[[nodiscard]] inline std::string BoneName(int headIndex)
{
    return headIndex <= 0 ? std::string("Head") : "Seg" + Pad2(headIndex);
}

/// 部位名。`E_<材質>_<部位>` の接尾辞になる (boss-serpent.md「命名」)。
[[nodiscard]] inline std::string PartSuffix(int headIndex)
{
    return headIndex <= 0 ? std::string("Head") : "S" + Pad2(headIndex);
}

/// 当たり判定オブジェクトの名前。
[[nodiscard]] inline std::string HitboxName(int headIndex)
{
    return "HB_" + BoneName(headIndex);
}

/// 部位名から頭からの添字へ。読めなければ -1。
[[nodiscard]] inline int IndexFromSuffix(std::string_view suffix)
{
    if (suffix == "Head") return 0;
    if (suffix.size() != 3 || suffix[0] != 'S') return -1;
    if (suffix[1] < '0' || suffix[1] > '9' || suffix[2] < '0' || suffix[2] > '9') return -1;
    const int value = (suffix[1] - '0') * 10 + (suffix[2] - '0');
    return (value >= 1 && value <= kSegmentCount) ? value : -1;
}

/// 節 1 本の当たり半径。前 (S06〜S07) が最も太く、尾へ細る (boss-serpent.md「体」)。
///
/// @note 等間隔に細らせない。一定割合で細るとホースに見えるため、モデルの太さ
///       (前を重くして «持ち上げている» と読める形) に当たりの半径も合わせる。
[[nodiscard]] inline float SegmentRadius(int headIndex, float neck, float peak, float tail)
{
    if (headIndex <= 0) return peak;
    constexpr float kPeakAt = 6.5f;
    const float i = static_cast<float>(headIndex);
    if (i <= kPeakAt)
        return fbzz::math::Lerp(neck, peak, fbzz::math::Clamp01((i - 1.0f) / (kPeakAt - 1.0f)));
    return fbzz::math::Lerp(peak, tail,
                            fbzz::math::Clamp01((i - kPeakAt) /
                                                (static_cast<float>(kSegmentCount) - kPeakAt)));
}

/// ローカル +Y を direction へ向ける回転。
///
/// @note 書き出された骨は子が必ずローカル (0, L, 0) に居るため、骨の +Y がそのまま
///       «次の関節への向き» になる。そこを合わせれば鎖が経路に乗る。
[[nodiscard]] inline fbzz::math::Quaternion AlignUpTo(const fbzz::math::Vector3& direction)
{
    using namespace fbzz::math;
    const Vector3 dir = direction.NormalizedOr(Vector3::UP);
    const float   dot = std::clamp(Vector3::Dot(Vector3::UP, dir), -1.0f, 1.0f);
    if (dot > 0.9999f)  return Quaternion::Identity();
    if (dot < -0.9999f) return Quaternion::FromAxisAngle(Vector3::RIGHT, PI);
    const Vector3 axis = Vector3::Cross(Vector3::UP, dir).NormalizedOr(Vector3::RIGHT);
    return Quaternion::FromAxisAngle(axis, std::acos(dot));
}

/// from を to へ重ねる最小の回転。
///
/// @note 関節ごとに前の骨からの最小回転で積む。向きだけ独立に解くとねじれの自由度が
///       残り、経路が少し動いただけで節が軸まわりに跳ねて背びれが暴れる。前から
///       送ればねじれは伝わるだけになり、胴が 1 本の鎖に見える。
[[nodiscard]] inline fbzz::math::Quaternion ShortestArc(const fbzz::math::Vector3& from,
                                                        const fbzz::math::Vector3& to)
{
    using namespace fbzz::math;
    const Vector3 a = from.NormalizedOr(Vector3::UP);
    const Vector3 b = to.NormalizedOr(Vector3::UP);
    const float   dot = std::clamp(Vector3::Dot(a, b), -1.0f, 1.0f);
    if (dot > 0.99999f) return Quaternion::Identity();
    if (dot < -0.99999f) {
        /// @note 真後ろ。軸が定まらないので a に直交する適当な軸で半回転する。
        Vector3 axis = Vector3::Cross(a, Vector3::UP);
        if (axis.LengthSq() < 1.0e-6f) axis = Vector3::Cross(a, Vector3::RIGHT);
        return Quaternion::FromAxisAngle(axis.NormalizedOr(Vector3::RIGHT), PI);
    }
    return Quaternion::FromAxisAngle(Vector3::Cross(a, b).NormalizedOr(Vector3::UP),
                                     std::acos(dot));
}

/// 2 つの向きのなす角 [deg]。折れ角の検算に使う。
[[nodiscard]] inline float AngleDegrees(const fbzz::math::Vector3& a, const fbzz::math::Vector3& b)
{
    using namespace fbzz::math;
    const float dot = std::clamp(Vector3::Dot(a.NormalizedOr(Vector3::UP),
                                              b.NormalizedOr(Vector3::UP)), -1.0f, 1.0f);
    return ToDeg(std::acos(dot));
}

} // namespace sandbox::serpent
