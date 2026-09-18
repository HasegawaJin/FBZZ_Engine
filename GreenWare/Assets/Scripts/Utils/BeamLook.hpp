/// @file    BeamLook.hpp
/// @brief   ボスの «線» の質。帯 2 層と放電の見た目をここ 1 つが決める
/// @author  Hasegawa Jin
/// @date    2026-09-06
///
/// @note ボスの薙ぎ (BossBeamComponent) と斉射 (LaserVolleyComponent) は 1 回の攻撃で
///       同時に出る (3 本中央がコアビーム、左右が斉射)。撃つ側が各自見た目を組むと
///       同じ攻撃の線が «筒と板» に割れて見えるため、断面・流れ・放電をここへ寄せる。
///       BeamTrailRendererComponent::PushMaterial は断面と流れを無条件に per-instance で
///       書き込み BeamTrailStyle の既定値が .mat を上書きするため、線の質の正本は
///       .mat ではなくこちら。.mat 側の同じキーはスクリプトを外したときのフォールバック。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/BeamTrailRendererComponent.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace fbzz::math;
using fbzz::Time;

namespace sandbox::beamlook {

/// 位相と模様の送りを巻き取る周期。シェーダーの frac(sin(x * 12.9898)) は x が
/// 大きくなるほど精度を失い、放置すると乱れが縞へ潰れる。
/// 整数で巻けば、巻き戻ったフレームでも絵は 1 ドットも動かない。
inline constexpr float kPhaseWrap  = 128.0f;
inline constexpr float kScrollWrap = 1024.0f;

/// 帯の揺れの既定の鍵。芯と裾には必ず同じ値を渡す (同じ波を共有する)。
inline constexpr uint32_t kSeed = 5011u;

/// 全部の線が差す .mat。«質» が 1 つである以上、差す先も 1 つ。
inline constexpr const char* kMaterial = "Assets/Materials/Effects/FX_BOSS_Beam.mat";

/// 線 1 本ぶんの状態。撃つ側が毎フレーム埋める。
///
/// @note 太さは «芯と裾» の 2 つで受ける。裾は芯のまわりの光であって別の線ではないが、
///       断面を解くときの基準は層ごとの実寸でなければならない (BeamTrailStyle::tubeRadius)。
struct Look {
    /// HDR の線色。明るさ込みで渡す。
    Vector4     color{ 1.0f, 0.12f, 0.14f, 1.0f };
    float       coreWidth    = 0.62f;
    float       glowWidth    = 1.90f;
    int         tubeSegments = 12;
    /// 点火 0..1。断面の «張り» と明るさの元になる。
    float       charge       = 1.0f;
    /// 10m 先での帯の振れ幅 [m]。ボスの線は «変わらない» のが読みなので浅く。
    float       wobble       = 0.05f;
    float       scrollSpeed  = 2.2f;
    float       churnRate    = 1.4f;
    uint32_t    seed         = kSeed;
    std::string materialPath = kMaterial;

    /// @name 放電と光
    /// @{
    /// @note 断面だけでなく «描き込みの量» (束の数・幅・たわみ) と点光源もここへ持つ。
    ///       断面と色を揃えても隣り合う線の «荒れ方» や明るさ・範囲が違うと、同じ攻撃の
    ///       線が割れて見える。線に沿って這わせる束の数。予算 (斉射の Bundle Budget) の
    ///       範囲で尊重される。
    int   arcAlong     = 3;
    int   arcStrands   = 2;
    float arcWidth     = 0.085f;
    float arcRate      = 26.0f;
    float arcIntensity = 2.0f;
    /// 筋が筒からどれだけ外へ膨らむか [m]。
    float arcBow       = 1.1f;
    /// 線に添える点光源。
    float lightIntensity = 24.0f;
    float lightRange     = 9.0f;
    /// @}
};

/// 点火の «張り»。太りきる直前がいちばん張っている、という出方にする。
[[nodiscard]] inline float Unrest(float charge01)
{
    const float ignite = Clamp01(charge01);
    return ignite * (1.0f - ignite) * 4.0f;
}

/// 明るさを 1 に正規化した色。«色が意味を持つ» 側 (放電・光) だけが要る値。
[[nodiscard]] inline Vector4 Hue(const Vector4& color)
{
    const float peak = std::max({ color.x, color.y, color.z, 1.0e-4f });
    return { color.x / peak, color.y / peak, color.z / peak, 1.0f };
}

/// 射線を伸ばし、最初に当たった «地形» で止める。何にも当たらなければ range で切る。
///
/// @note «どこで止まるか» も線の質のうちなので撃つ側ごとに書かず、断面や放電と同じく
///       ここが正本になる (中央だけ面で止まり左右が宙で切れると同じ口から出た線に
///       見えなくなる)。最初の 1 件ではなく全件を見る ─ 間にプレイヤーが立つと
///       Raycast はそこで止まるが、当たり判定は線分の側で別に測っているため、
///       線そのものは地形だけで止める。
[[nodiscard]] inline Vector3 TraceSurface(const fbzz::scene::Script& owner, const Vector3& from,
                                          const Vector3& direction, float range,
                                          const std::string& playerTag, Vector3& outNormal)
{
    outNormal = Vector3::UP;

    const float   reach = std::max(range, 0.5f);
    const Vector3 dir   = direction.NormalizedOr(Vector3{ 0.0f, -1.0f, 0.0f });

    fbzz::scene::GameObject* self = owner.scene.Self();
    const fbzz::scene::RaycastHit* nearest = nullptr;
    const std::vector<fbzz::scene::RaycastHit> hits = owner.physics.RaycastAll(from, dir, reach);
    for (const fbzz::scene::RaycastHit& hit : hits) {
        fbzz::scene::GameObject* object = hit.gameObject;
        if (!object) continue;
        /// @note 自分の部位 (脚のヒットボックス) で止まると、線が脚の上で切れる。
        if (object == self || (self && object->IsDescendantOf(*self))) continue;
        if (!playerTag.empty() && object->tag == playerTag) continue;
        if (!nearest || hit.distance < nearest->distance) nearest = &hit;
    }

    if (nearest) {
        outNormal = nearest->normal.NormalizedOr(Vector3::UP);
        return nearest->point;
    }
    /// @note 何にも当たらないまま抜けた。線は最大距離で切る (空へ伸ばしっぱなしにしない)。
    return from + dir * reach;
}

/// axis に垂直な 2 軸。放電を «軸のまわり» や «面の上» へ散らすのに使う。
inline void PerpendicularBasis(const Vector3& axis, Vector3& outSide, Vector3& outUp)
{
    /// @note 軸が真上に近いときだけ基準を前方へ倒す (外積が縮退するため)。
    const Vector3 reference = std::fabs(axis.y) > 0.9f ? Vector3::FORWARD : Vector3::UP;
    outSide = Vector3::Cross(axis, reference).NormalizedOr(Vector3::RIGHT);
    outUp   = Vector3::Cross(outSide, axis).NormalizedOr(Vector3::UP);
}

/// 帯 1 層ぶんの見た目を組む。
///
/// @note 板ではなく筒で張る。板は区間ごとに 1 枚の四角形をカメラへ向けるだけなので、
///       太いものほど紙に見える。筒なら輪郭も深度も実体が持つので、床や壁へ突き刺さる
///       «質量» が読める。断面の厚みはシェーダーが視線と円柱を交差させて出す
///       (BossBeam.hlsl の BossBeamCylinder)。
[[nodiscard]] inline BeamTrailStyle Style(const Look& look, bool isCore)
{
    const float ignite = Clamp01(look.charge);

    BeamTrailStyle style;
    style.materialPath = look.materialPath;
    style.isCore = isCore;
    style.width  = isCore ? std::max(look.coreWidth, 0.01f) : std::max(look.glowWidth, 0.01f);
    style.color  = look.color;

    style.shape          = LineShape::Tube;
    style.radialSegments = std::clamp(look.tubeSegments, 3, 32);
    style.tubeRadius     = style.width * 0.5f;
    /// @note 裾を芯より手前に描く。逆にすると裾のアルファが芯を薄めて線が濁る。
    style.orderInLayer = isCore ? 2 : 1;

    /// @note 帯の揺れ。芯と裾には同じ seed の同じ波を渡し、裾だけ浅くして «芯を鈍く追う» にする。
    style.seed        = look.seed;
    style.wobble      = std::max(look.wobble, 0.0f);
    style.wobbleScale = isCore ? 1.0f : 0.6f;
    style.wobbleFrequency = 2.0f;
    style.wobbleTravel    = 1.1f;
    style.wobbleBias      = 0.6f;

    /// @name 断面
    style.coreWidth   = 0.30f;
    style.edgeFalloff = 2.0f;
    style.coreBoost   = 2.6f;
    style.muzzleFade  = 0.05f;
    style.tipFade     = 0.04f;
    style.beadDensity = 7.0f;
    style.beadFalloff = 9.0f;
    style.arcFreq     = 5.0f;

    /// @note 点火中だけ荒れさせる。太りきった線が暴れていると «出力が不安定» に見えて、
    ///       «避けるしかない» という性格が薄まる。
    const float unrest = Unrest(ignite);
    style.arcAmp  = 0.06f + 0.20f * unrest;
    style.crackle = 0.18f + 0.45f * unrest;
    style.flicker = 0.12f + 0.35f * unrest;

    /// @note 流れ。tiling は長さで割らない ─ BeamTrailRendererComponent が長さを掛けるので、
    ///       ここは «1m あたり何回繰り返すか» を渡す。
    style.tiling = 3.0f;
    style.scroll = std::fmod(-Time::time * look.scrollSpeed, kScrollWrap);
    style.phase  = std::fmod(Time::time * std::max(look.churnRate, 0.0f), kPhaseWrap);
    style.surge  = unrest;
    return style;
}

/// 線に付く放電の基本。膨らみ方と長さだけ呼び出し側が変える。
[[nodiscard]] inline ElectricArcStyle ArcStyle(const Look& look, float brightness,
                                               int strands, float width, float rate,
                                               float intensity)
{
    const Vector4 hue = Hue(look.color);

    ElectricArcStyle style;
    style.strandCount = std::clamp(strands, 1, 6);
    style.width       = std::max(width, 0.001f);
    style.strikeRate  = std::max(rate, 1.0f);
    style.intensity   = std::max(intensity, 0.0f) * brightness;
    /// @note ビームより手前に出す。筒の «外» を這っていることが分かる並びにする。
    style.orderInLayer = 3;
    /// @note 距離での減衰は切る。線は 20m を超えることがあり、既定の 7m だと
    ///       «長く撃つほど放電だけ消える» という読めない挙動になる。
    style.strikeRange = 0.0f;
    style.breakup     = 0.5f;
    style.travel      = 9.0f;
    /// @note «色が意味を持つ» を保つ。両端とも線の色にして、芯だけ白熱させる。
    style.fromColor   = hue;
    style.toColor     = hue;
    style.coreTint    = 0.35f;
    return style;
}

} // namespace sandbox::beamlook
