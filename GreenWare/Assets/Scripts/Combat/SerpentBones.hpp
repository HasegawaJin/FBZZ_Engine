/// @file    SerpentBones.hpp
/// @brief   蛇の骨と部位の名前、鎖を経路へ沿わせるための回転。アタッチしないユーティリティ
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY 1 箇所へ集めるか:
///   «Seg07 の骨» と «S07 の部位» と «HB_Seg07 の当たり» は同じ 1 つの節を指しているのに、
///   綴りが別々の場所にあると、片方だけ直したときの症状が «その節だけ当たらない» /
///   «その節だけ輪郭が出ない» という、絵を見ても原因の分からない形で出る。
///   骨を並べる側 (Spine)・当たりを生やす側 (HitboxRig)・輪郭を描く側 (PolarityRig) が
///   同じ関数から名前を引く。
///
/// WHY 頭からの添字にするか:
///   骨の親子は尾が根 (Root → Seg28 → … → Seg01 → Head) だが、経路を進むのは頭で、
///   節が減るのも «頭から数えて何本目» で効く。番号の向きを 2 つ持つと、
///   ループのたびにどちらの向きだったかを読み直すことになる。
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
/// WHY 等間隔に細らせないか: 一定の割合で細らせるとホースに見える。前を重くして
///     «持ち上げている» が読める形にしたモデルの太さに、当たりの側も合わせる。
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
/// WHY +Y か: 書き出された骨は子が必ずローカル (0, L, 0) に居る。つまり «次の関節へ
///     向かう向き» が骨の +Y そのもので、そこを合わせれば鎖が経路に乗る。
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
/// WHY 関節ごとに «前の骨からの差分» で積むか: 向きだけを合わせる回転にはねじれの
///     自由度が残る。関節ごとに独立に解くと、経路が少し動いただけで節が軸まわりに
///     跳ね、背びれが毎フレーム別の方向を向く。前の骨から最小回転で送れば
///     ねじれは伝わるだけになり、胴が «1 本の鎖» に見える。
[[nodiscard]] inline fbzz::math::Quaternion ShortestArc(const fbzz::math::Vector3& from,
                                                        const fbzz::math::Vector3& to)
{
    using namespace fbzz::math;
    const Vector3 a = from.NormalizedOr(Vector3::UP);
    const Vector3 b = to.NormalizedOr(Vector3::UP);
    const float   dot = std::clamp(Vector3::Dot(a, b), -1.0f, 1.0f);
    if (dot > 0.99999f) return Quaternion::Identity();
    if (dot < -0.99999f) {
        // 真後ろ。軸が定まらないので a に直交する適当な軸で半回転する。
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
