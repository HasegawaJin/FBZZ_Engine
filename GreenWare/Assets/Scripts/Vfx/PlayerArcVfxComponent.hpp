/// @file    PlayerArcVfxComponent.hpp
/// @brief   剣を振っている間、プレイヤーの体を極性色の放電が纏う
/// @author  Hasegawa Jin
/// @date    2026-09-03
///
/// WHY 発光色の差し替えだけでは足りないか:
///   PlayerPolarityComponent は纏っている間 GlowPart の色を極性色へ置き換えるが、
///   プレイヤーの発光は面積が小さく、TPS の引き距離では «緑が赤くなった» 程度しか
///   出ない。«帯びている» が伝わるには、体の輪の外へ出る形が要る。
///
/// WHY 放電を «2 点間» で描くか:
///   ElectricArcBundle は端点だけを受ける (Utils/ElectricArc.hpp)。体に沿って
///   帯を貼るのではなく、刃と刃・刃と胸を結ぶ形にすると、腕がどこにあっても
///   «その 2 点の間» として成立する ── 姿勢ごとの調整が要らない。
///
/// WHY 剣を持っていないときも成立させるか:
///   ソケットは抜刀状態でしか付いていないことがある。端点が引けなければその束だけ
///   消灯し、残りは走らせる。«片方だけ出ない» で済ませて、全部消えるのを避ける。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Player/PlayerPolarityComponent.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerArcVfxComponent : public Script {
    FBZZ_SCRIPT(PlayerArcVfxComponent)

public:
    FBZZ_GROUP("Arc")
    FBZZ_FIELD(bool, showArcs, true, "Show Arcs")
    FBZZ_TOOLTIP("振っている間、体に放電を走らせる。切ると発光色の変化だけになる")
    FBZZ_FIELD_RANGE_INT(int, strandCount, 3, "Strands", 1, 8)
    FBZZ_TOOLTIP("1 束あたりの筋の数。1 本だと «線» に見えるので束ねる")
    FBZZ_FIELD_RANGE(float, amplitude, 0.28f, "Amplitude", 0.0f, 1.5f)
    FBZZ_TOOLTIP("折れの振れ幅 [m]。体に纏う放電なので、電極間の放電より小さく取る")
    FBZZ_FIELD_RANGE(float, width, 0.05f, "Width", 0.005f, 0.5f)
    FBZZ_FIELD_RANGE(float, intensity, 2.2f, "Intensity", 0.0f, 12.0f)
    FBZZ_TOOLTIP("1 を超えるとブルームが拾う。筋は細いので白飛びしにくい")
    FBZZ_FIELD_RANGE(float, strikeRate, 26.0f, "Strike Rate", 1.0f, 60.0f)
    FBZZ_TOOLTIP("形を組み替える頻度 [Hz]。上げすぎると稲妻ではなく雑音の帯になる")
    FBZZ_FIELD_RANGE(float, coreTint, 0.55f, "Core Tint", 0.0f, 1.0f)
    FBZZ_TOOLTIP("芯を極性色へ寄せる量。0 で純白の芯、1 で完全に赤 / 青")

    // 帯びている時間の «終わり» を絵で出す。
    //
    // WHY 残り比で細らせるか: 纏いは 0.9 秒で切れる (bladeChargeSeconds)。同じ強さで
    //     出しっぱなしにすると、消える瞬間が «ぶつ切り» になって «切れた» ではなく
    //     «バグで消えた» に見える。
    FBZZ_FIELD_RANGE(float, fadeCurve, 0.6f, "Fade Curve", 0.1f, 3.0f)
    FBZZ_TOOLTIP("残り時間に対する明るさの落ち方。1 で線形、小さいほど最後まで粘る")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugLiveArcs, 0, "Live Arcs")

    /// PlayerComponent が注入する。«いつ帯びているか» を持つのはあちら。
    void SetPolarity(PlayerPolarityComponent* polarity) { m_polarity = polarity; }

    void OnStart()   override;
    void OnUpdate()  override;
    /// 消えるときに筋を残さない。放電の GameObject は実行時生成なので、
    /// 持ち主が居なくなっても Scene 側に居残る。
    void OnDestroy() override;

private:
    /// 放電の本数。刃どうし / 右刃と胸 / 左刃と胸 の 3 束。
    static constexpr int kBundleCount = 3;

    [[nodiscard]] PlayerPolarityComponent* Polarity() const
    { return m_polarity ? m_polarity : scene.GetScript<PlayerPolarityComponent>(); }

    /// 名前でソケットを引く。見つからなければ nullptr。
    [[nodiscard]] GameObject* Socket(const char* name) const;
    /// 胸のあたり。ソケットが無い構成でも成立させるため、足元から高さで作る。
    [[nodiscard]] Vector3 ChestPoint() const;

    void Extinguish();

    PlayerPolarityComponent* m_polarity = nullptr;
    ElectricArcBundle        m_bundles[kBundleCount];
    bool                     m_lit = false;
};

FBZZ_REFLECT(PlayerArcVfxComponent)

inline void PlayerArcVfxComponent::OnStart()
{
    // 鍵は持ち主ごとに固有でなければならない。同じ鍵の束が 2 つあると、
    // 同じ筋の GameObject を奪い合って片方が消える (ElectricArc.hpp の WHY)。
    const unsigned    id   = scene.Self() ? scene.Self()->GetID().index : 0u;
    const std::string self = std::to_string(id);
    for (int i = 0; i < kBundleCount; ++i)
        m_bundles[i].SetKey("PlayerCoat" + self + "_" + std::to_string(i));
}

inline void PlayerArcVfxComponent::OnDestroy()
{
    for (ElectricArcBundle& bundle : m_bundles) bundle.Detach(*this);
}

inline GameObject* PlayerArcVfxComponent::Socket(const char* name) const
{
    GameObject* self = scene.Self();
    if (!self || !name) return nullptr;
    return FindInSubtree(*self, name);
}

inline Vector3 PlayerArcVfxComponent::ChestPoint() const
{
    // 全高 2.51m のうち胸はおよそ 7 割の高さ。ボーンを探しに行かないのは、
    // 骨名がモデルの都合で変わるのに対し «体の高さの比» は変わらないため。
    Vector3 point = transform.worldPosition;
    point.y += 1.75f;
    return point;
}

inline void PlayerArcVfxComponent::Extinguish()
{
    if (!m_lit) return;
    for (ElectricArcBundle& bundle : m_bundles) bundle.Extinguish(*this);
    m_lit         = false;
    debugLiveArcs = 0;
}

inline void PlayerArcVfxComponent::OnUpdate()
{
    if (!enabled || !showArcs) { Extinguish(); return; }

    const auto* polarity = Polarity();
    if (!polarity || !polarity->IsCharged()) { Extinguish(); return; }

    const float dt    = Max(Time::deltaTime, 0.0f);
    const float ratio = Clamp01(polarity->ChargeRatio());
    // 残りに対して緩く落とす。線形だと «切れる» が最後の 1 フレームに集まる。
    const float fade  = std::pow(ratio, Max(fadeCurve, 0.1f));

    const Vector4 color = PolarityColor(polarity->Current());

    ElectricArcStyle style;
    style.strandCount = std::max(strandCount, 1);
    style.segments    = 18;
    style.amplitude   = Max(amplitude, 0.0f) * fade;
    style.width       = Max(width, 0.005f);
    style.strikeRate  = Max(strikeRate, 1.0f);
    // 体に纏う放電なので距離で消さない。腕を伸ばせば端点は 2m 近く離れる。
    style.strikeRange = 0.0f;
    style.intensity   = Max(intensity, 0.0f) * fade;
    style.coreTint    = Clamp01(coreTint);
    style.breakup     = 0.7f;
    style.travel      = 9.0f;
    style.fromColor   = color;
    style.toColor     = color;

    GameObject* right = Socket(kSocketKatanaR);
    GameObject* left  = Socket(kSocketKatanaL);
    const Vector3 chest = ChestPoint();

    int live = 0;
    const auto run = [&](int index, const Vector3& from, const Vector3& to) {
        m_bundles[index].Update(*this, from, to, style, dt);
        ++live;
    };

    // 刃どうし。両手の間を渡る筋が «2 本の剣が同じ極を帯びている» を一番強く出す。
    if (right && left)
        run(0, right->transform.worldPosition, left->transform.worldPosition);
    else
        m_bundles[0].Extinguish(*this);

    // 体から刃へ。片方しか引けなくても、残った側は走らせる。
    if (right) run(1, chest, right->transform.worldPosition);
    else       m_bundles[1].Extinguish(*this);

    if (left) run(2, chest, left->transform.worldPosition);
    else      m_bundles[2].Extinguish(*this);

    m_lit         = true;
    debugLiveArcs = live;
}

} // namespace sandbox
