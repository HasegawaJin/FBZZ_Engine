/// @file    BladeChargeGlowComponent.hpp
/// @brief   双剣の芯を «溜めている量» で光らせる。溜めを剣そのもので伝える層
/// @author  Hasegawa Jin
/// @date    2026-09-06
///
/// シーンへは付けない。PlayerComponent が内部モジュールとして持つ。
/// BladeComponent へ問い合わせるだけで、剣の挙動は一切触らない。
///
/// @note 2026-09-06: 溜め中の震え・パッド・歪み・音程 (DriveCharge) はあるのに刀自体は
///       変わらなかった穴を埋める。軌跡で封じた極性色をあえて芯で使うのは、左右どちらが
///       光るかで «次に乗る極» を伝える限られた場所だから。明滅はさせず、量は明るさの
///       絶対値、上限到達はブルームのしきい値越え (GlowPartComponent と同じ理由)。スロットは
///       番号でなく名前 (M_SwordCore_* / M_SwordMark_*) で拾い、FBX 差し替えでの事故を防ぐ。
#pragma once

#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Player/BladeComponent.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BladeChargeGlowComponent : public Script {
    FBZZ_SCRIPT(BladeChargeGlowComponent)

public:
    FBZZ_GROUP("Blade Charge Glow")
    FBZZ_FIELD_RANGE(float, bladeGlowIdle, 0.14f, "待機", 0.0f, 4.0f)
    FBZZ_TOOLTIP("溜めていないときの芯の明るさ。0 で «普段は光らない刀» になる。"
                 "少しだけ点けておくと、溜めたときの伸びが «点いた» ではなく «強まった» に読める")
    FBZZ_FIELD_RANGE(float, bladeGlowFull, 2.6f, "Charged", 0.0f, 12.0f)
    FBZZ_TOOLTIP("満溜めの明るさ。ブルームのしきい値 (4.0) より下に置くこと ─ "
                 "ここで既に滲んでいると、下の Full Flash が «跨いだ» ことを言えなくなる")
    FBZZ_FIELD_RANGE(float, bladeGlowFlash, 5.6f, "Full Flash", 0.0f, 20.0f)
    FBZZ_TOOLTIP("満溜めに «届いた瞬間» だけの明るさ。しきい値を越えるので刀が滲む。"
                 "耳 (kBladeChargeUpFull) と手 (Rumble) で既に鳴っている合図の絵側")
    FBZZ_FIELD_RANGE(float, bladeGlowFlashSeconds, 0.14f, "Full Flash Time", 0.0f, 0.6f)

    FBZZ_FIELD_RANGE(float, bladeGlowSwing, 2.2f, "Swing Flare", 0.0f, 12.0f)
    FBZZ_TOOLTIP("振った瞬間に振った側の刀が灯る明るさ。0 で出さない。"
                 "軌跡 (BladeTrail) が出るのは刃の «跡» なので、刀身そのものは別に灯す")
    FBZZ_FIELD_RANGE(float, bladeGlowSwingSeconds, 0.20f, "Swing Flare Time", 0.0f, 0.8f)

    /// @note 震え・パッド・歪みは比の 2 乗で立ち上がる (DriveCharge) ため、発光も同じ曲線に
    ///       乗せる。線形だと前半 «光っているのに何も起きない»、終盤 «手だけ来て絵が
    ///       追いつかない» になる。
    FBZZ_FIELD_RANGE(float, bladeGlowCurve, 2.0f, "Curve", 0.5f, 4.0f)
    FBZZ_TOOLTIP("溜め比に掛ける指数。2 で DriveCharge の震え・パッドと同じ立ち上がり")

    FBZZ_GROUP("Blade Charge Glow — 光らせるスロット")
    FBZZ_FIELD(std::string, bladeGlowMatch, "SwordCore", "Match A")
    FBZZ_TOOLTIP("この文字列を .mat のパスに含むスロットを光らせる")
    FBZZ_FIELD(std::string, bladeGlowMatch2, "SwordMark", "Match B")
    FBZZ_TOOLTIP("2 つ目の条件。空なら A だけ")

    FBZZ_FIELD_READ_ONLY(int, debugBladeGlowSlots, 0, "Glow Slots")

    /// 溜めの量と «どちらの剣か» はここから読む。注入されないと何もしない。
    void SetBlades(BladeComponent* blades) { m_blades = blades; }

    void OnStart()  override;
    void OnUpdate() override;

private:
    /// 光らせる 1 スロット。元の自発光を控えて、溜めが切れたら必ず戻す。
    struct Slot {
        EntityRef target;
        uint32_t  slot = 0;
        Vector3   baseColor{ 0.0f, 0.0f, 0.0f };
        float     baseScale = 0.0f;
    };

    /// 刀 1 振りぶんの状態。
    struct Blade {
        std::vector<Slot> slots;
        /// 振った直後の灯り [秒]。
        float flare = 0.0f;
        /// 今フレーム自発光を上書きしたか。戻し忘れを 1 つの札で防ぐ。
        bool  writing = false;
    };

    [[nodiscard]] static std::size_t HandIndex(HandSide hand)
    { return hand == HandSide::Right ? 0u : 1u; }

    /// その手の刀の «光らせるスロット» を集め直す。抜刀で後から現れるので、
    /// 空のあいだは毎フレーム探しに行く (見つかった時点で止まる)。
    void Collect(HandSide hand);
    /// 1 振りへ書く。level <= 0 なら控えた元の値へ戻して、書いたことを忘れる。
    void Drive(HandSide hand, const Vector4& color, float level);
    [[nodiscard]] bool Matches(const std::string& path) const;

    BladeComponent* m_blades = nullptr;
    Blade m_hands[2];
    /// 前フレームの溜め比と «振っていたか»。立ち上がりの 1 フレームを取るため。
    float m_lastRatio   = 0.0f;
    bool  m_wasSwinging = false;
    /// 満溜めに届いた合図の残り [秒]。左右で分けないのは、溜めているのが常に
    /// 片手だけ (または Flux で両手同時) だから。
    float m_fullFlash   = 0.0f;
};

FBZZ_REFLECT(BladeChargeGlowComponent)


inline void BladeChargeGlowComponent::OnStart()
{
    for (Blade& blade : m_hands) {
        blade.slots.clear();
        blade.flare   = 0.0f;
        blade.writing = false;
    }
    m_lastRatio   = 0.0f;
    m_wasSwinging = false;
    debugBladeGlowSlots = 0;
}

inline bool BladeChargeGlowComponent::Matches(const std::string& path) const
{
    if (path.empty()) return false;
    if (!bladeGlowMatch.empty() && path.find(bladeGlowMatch) != std::string::npos)
        return true;
    return !bladeGlowMatch2.empty() && path.find(bladeGlowMatch2) != std::string::npos;
}

inline void BladeChargeGlowComponent::Collect(HandSide hand)
{
    Blade& blade = m_hands[HandIndex(hand)];
    blade.slots.clear();

    GameObject* sword = scene.Find(SwordObjectName(hand));
    if (!sword) return;

    auto* materials = sword->GetComponent<MaterialComponent>();
    if (!materials) return;

    /// @note スロット 0 は MaterialComponent 自身、1 以降が extraSlots (ScriptMaterialProxy の
    ///       GetSlotCount と同じ数え方)。番号ではなく .mat のパスで選ぶ。
    const std::size_t count = materials->SlotCount();
    for (std::size_t i = 0; i < count; ++i) {
        if (!Matches(materials->RawSlotAt(i).materialPath)) continue;

        Slot entry{ EntityRef{ sword->GetID() }, static_cast<uint32_t>(i),
                    Vector3::ZERO, 0.0f };
        const MaterialInstance instance = material.Instance(entry.target, entry.slot);
        if (!instance.IsValid()) continue;
        /// @note 元の値が読めない材質もある (自発光を持たない)。0 から始めて 0 へ戻す。
        (void)instance.TryGetVector3(kEmissiveColorId, entry.baseColor);
        (void)instance.TryGetFloat(kEmissiveScaleId, entry.baseScale);
        blade.slots.push_back(entry);
    }
}

inline void BladeChargeGlowComponent::Drive(HandSide hand, const Vector4& color, float level)
{
    Blade& blade = m_hands[HandIndex(hand)];

    const bool want = level > 0.0f;
    /// @note 書いていないなら戻すものも無い
    if (!want && !blade.writing) return;

    for (const Slot& entry : blade.slots) {
        const MaterialInstance instance = material.Instance(entry.target, entry.slot);
        if (!instance.IsValid()) continue;
        if (want) {
            instance.SetVector3(kEmissiveColorId, Vector3{ color.x, color.y, color.z });
            instance.SetFloat(kEmissiveScaleId, level);
        } else {
            instance.SetVector3(kEmissiveColorId, entry.baseColor);
            instance.SetFloat(kEmissiveScaleId, entry.baseScale);
        }
    }
    blade.writing = want;
}

inline void BladeChargeGlowComponent::OnUpdate()
{
    if (!enabled || !m_blades) return;

    const float dt = Max(Time::deltaTime, 0.0f);

    /// @note 抜刀で後から現れる。空のあいだだけ探しに行き、見つかったら止まる。
    const HandSide hands[] = { HandSide::Right };
    int found = 0;
    for (const HandSide hand : hands) {
        if (m_hands[HandIndex(hand)].slots.empty()) Collect(hand);
        found += static_cast<int>(m_hands[HandIndex(hand)].slots.size());
    }
    debugBladeGlowSlots = found;

    /// @name 振った瞬間
    /// @note 立ち上がりで取る。振っている «あいだ» 灯し続けると硬直中も光ったままで
    ///       «次が振れる» と読み違えるため、灯りは一撃ごとに 1 回。
    const bool swinging = m_blades->IsSwinging();
    if (swinging && !m_wasSwinging) {
        const BladeSide swung = m_blades->SwingSide();
        /// @note 溜め斬りは体ごと回る全周の一撃なので、枠を左右とも灯す。刀は 1 本なので
        ///       片方は空振りするが、灯し損ねるより害が無い (両手剣へ替えた名残)。
        if (m_blades->IsCharged()) {
            m_hands[0].flare = 1.0f;
            m_hands[1].flare = 1.0f;
        } else if (swung != BladeSide::None) {
            m_hands[HandIndex(HandOf(swung))].flare = 1.0f;
        }
    }
    m_wasSwinging = swinging;

    /// @name 溜め
    const float    ratio   = Clamp01(m_blades->ChargeRatio());
    const BladeSide holding = m_blades->ChargingSide();
    /// @note 満溜めへ «届いた» 1 フレーム。耳と手には既に合図が出ている (NotifyChargeFull)。
    const bool     reached = ratio >= 1.0f && m_lastRatio < 1.0f;
    m_lastRatio = ratio;

    if (reached) m_fullFlash = Max(bladeGlowFlashSeconds, 0.0f);
    m_fullFlash = Max(m_fullFlash - dt, 0.0f);

    const float charged = std::pow(ratio, Max(bladeGlowCurve, 0.01f)) * Max(bladeGlowFull, 0.0f);
    const float flash   = bladeGlowFlashSeconds > 0.0f
        ? Max(bladeGlowFlash, 0.0f) * (m_fullFlash / Max(bladeGlowFlashSeconds, 1.0e-3f))
        : 0.0f;

    for (const HandSide hand : hands) {
        Blade& blade = m_hands[HandIndex(hand)];
        blade.flare = bladeGlowSwingSeconds > 0.0f
            ? Max(blade.flare - dt / Max(bladeGlowSwingSeconds, 1.0e-3f), 0.0f)
            : 0.0f;

        /// @note 溜めている剣«だけ»を光らせる。左右どちらが光るかが «次にどちらの極が
        ///       乗るか» そのものなので、両方光らせると情報が消える。Flux (押していない
        ///       満溜め) だけはまだどちらを振るか決まっていないため両方灯し «どちらでも
        ///       いい» を表す。
        const bool charging = holding != BladeSide::None
                            ? (HandOf(holding) == hand) : (ratio > 0.0f);

        /// @note 灯りは «足す» のではなく最も明るいものを採る。足すと溜め切って振った
        ///       瞬間だけ 2 段ぶん跳ねて、そこだけ白飛びする。
        float level = Max(bladeGlowIdle, 0.0f);
        if (charging) level = Max(level, Max(charged, flash));
        level = Max(level, Max(bladeGlowSwing, 0.0f) * blade.flare);

        /// @note 色は «その剣の» 極。左右で固定なので、溜めていないときの控えめな灯りでも
        ///       どちらが ＋ でどちらが − かが常に読める。
        ///       刃は左右を名乗らない (BladeColors.hpp の PlayerBladeColor を参照)。
        Vector4 color = PlayerBladeColor();
        (void)hand;
        /// @note 満溜めの瞬間だけ白へ寄せる。極性色のまま明るくすると、赤は «もっと赤い» に
        ///       しかならず «上限に着いた» が出ない。
        if (charging && flash > 0.0f) {
            const float toWhite = Clamp01(flash / Max(bladeGlowFlash, 1.0e-3f));
            color = Vector4{ Lerp(color.x, 1.0f, toWhite), Lerp(color.y, 1.0f, toWhite),
                             Lerp(color.z, 1.0f, toWhite), 1.0f };
        }

        Drive(hand, color, level);
    }
}

} // namespace sandbox
