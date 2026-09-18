/// @file    BossPartTelegraphComponent.hpp
/// @brief   «次に何が来るか» を、床ではなくボスの体の光る部位で言う
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// @note 床の予兆 (円/線の 2 形、BossTelegraph.hpp) だけでは近距離で本体に隠れて見えない
///       ため、攻撃ごとに «ボスの体のどの部位が光るか» で区別する。
/// @note 色は場所でのみ区別し攻撃ごとに変えない (赤青=極性/琥珀=危険で企画書 12.2 が既に
///       埋めている)。極性色 (コア/リング、BossCoreComponent 担当) も上書きしない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/BossAnimParams.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossPartTelegraphComponent : public Script {
    FBZZ_SCRIPT(BossPartTelegraphComponent)

public:
    FBZZ_GROUP("Parts")
    FBZZ_REF_LIST_FIELD(GameObject, frontRightParts, "Front Right")
    FBZZ_TOOLTIP("右前脚の発光メッシュ。踏みつけと突進で光る")
    FBZZ_REF_LIST_FIELD(GameObject, frontLeftParts, "Front Left")
    FBZZ_REF_LIST_FIELD(GameObject, backRightParts, "Back Right")
    FBZZ_REF_LIST_FIELD(GameObject, backLeftParts, "Back Left")
    FBZZ_REF_LIST_FIELD(GameObject, headParts, "頭")
    FBZZ_TOOLTIP("突進で光る頭部。ビームはコア側の担当なのでここには入れない")
    FBZZ_REF_LIST_FIELD(GameObject, bodyParts, "Body")
    FBZZ_TOOLTIP("磁力パルスで全周が光る胴。極性リング (E_RingGlow) は入れないこと ─ "
                 "あちらは極性色の担当が別に居る")
    FBZZ_FIELD_RANGE_INT(int, materialSlot, 0, "Material Slot", 0, 15)

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(warnColor, (Vector4{ 1.00f, 0.62f, 0.14f, 1.00f }), "Warn Color")
    FBZZ_TOOLTIP("危険の琥珀。DecalBossTelegraph の frameColor と揃えること")
    FBZZ_FIELD_RANGE(float, peakScale, 6.0f, "Peak Scale", 0.0f, 30.0f)
    FBZZ_TOOLTIP("着弾直前の発光量。Bloom のしきい値を越える値にしないと滲まない")
    FBZZ_FIELD_RANGE_INT(int, pips, 3, "ピップの数", 0, 8)
    FBZZ_TOOLTIP("拍の数。床のデカールと同じ値にすること ─ 食い違うと足元と体が"
                 "別々のリズムで光り、どちらでタイミングを取るのか決められない")
    FBZZ_FIELD_RANGE(float, pipGain, 2.1f, "ピップの明滅", 1.0f, 4.0f)
    FBZZ_FIELD_RANGE(float, strikeFrom, 0.82f, "打撃の起点", 0.0f, 1.0f)
    FBZZ_TOOLTIP("回避窓の入口。ここから脈が止まって張り付く")
    FBZZ_FIELD_RANGE(float, releaseSeconds, 0.12f, "解放", 0.0f, 1.0f)
    FBZZ_TOOLTIP("予兆が消えてから光が引くまで。0 だと着弾の瞬間にパッと消えて «不発» に見える")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugKind, "None", "Kind")
    FBZZ_FIELD_READ_ONLY(float, debugGlow, 0.0f, "発光")

    void OnStart() override
    {
        m_ai = scene.GetScript<BossAiComponent>();
        if (!m_ai) debug.LogError("BossPartTelegraphComponent requires BossAiComponent "
                                  "on the same GameObject.");
    }

    void OnUpdate() override
    {
        if (!m_ai) return;

        const BossTelegraph& telegraph = m_ai->CurrentTelegraph();
        const bool active = telegraph.shape != BossTelegraphShape::None
                         && telegraph.kind  != BossAttackKind::None;

        /// @note 予兆が消えた後も少しだけ残す。着弾と同時に消えると «出なかった» に見える。
        if (active) {
            m_kind    = telegraph.kind;
            m_release = std::max(releaseSeconds, 0.0f);
        } else if (m_release > 0.0f) {
            m_release = std::max(m_release - Time::deltaTime, 0.0f);
        }

        const float progress = active ? Clamp01(telegraph.progress) : 1.0f;
        const float envelope = active
            ? progress
            : (releaseSeconds > 0.0f ? m_release / releaseSeconds : 0.0f);

        /// @note 拍は床のデカールと同じ型 (BossTelegraphCue) で刻む。`sin(Time::time * hz)`
        ///       の自前正弦波は絶対時刻ゆえ床と位相が食い違い、足元と体が別リズムで光った。
        ///       拍数も床と同じ値 (出す側が渡す) を使い、自前の既定は使わない。
        m_cue.pips         = telegraph.pips > 0 ? telegraph.pips : std::max(pips, 0);
        m_cue.pipGain      = pipGain;
        m_cue.strikeFrom   = Clamp01(strikeFrom);
        /// @note 消え際は releaseSeconds が持っている
        m_cue.burstSeconds = 0.0f;
        m_cue.Tick(progress, std::max(Time::deltaTime, 0.0f), active);

        /// @note 進みの二乗で立ち上げる。線形だと予兆の «前半» から明るく、
        ///       «あと少し» の情報が出ない。
        float glow = envelope * envelope * std::max(peakScale, 0.0f);
        /// @note 回避窓では «脈» をやめて張り付かせる。明滅が止まることが «今» の合図になる。
        if (active) glow *= m_cue.pulse * Lerp(1.0f, 1.35f, m_cue.strike);

        debugKind = KindName(m_kind);
        debugGlow = glow;

        Clear();
        if (glow > 0.0f) Light(m_kind, glow);
        Flush();
    }

private:
    struct Lit {
        EntityRef ref;
        float     glow = 0.0f;
    };

    /// 拍と回避窓。床のデカールと同じ型を通すことで、足元と体が同じリズムで光る。
    BossTelegraphCue m_cue;

    void Clear() { m_lit.clear(); }

    void Add(const std::vector<Ref<GameObject>>& parts, float glow)
    {
        for (const auto& part : parts) {
            if (!part.IsAssigned()) continue;
            m_lit.push_back({ part.ref, glow });
        }
    }

    /// 攻撃 → 光る部位。ここが «何が来るか» の対応表そのもの。
    void Light(BossAttackKind kind, float glow)
    {
        switch (kind) {
        case BossAttackKind::Stomp:
            Add(LegParts(m_ai->StompLeg()), glow);
            break;
        case BossAttackKind::Slam:
            /// @note 着地は四脚すべて。«どこへ逃げても踏まれる» を体で言う。
            Add(frontRightParts, glow); Add(frontLeftParts, glow);
            Add(backRightParts,  glow); Add(backLeftParts,  glow);
            break;
        case BossAttackKind::Charge:
            /// @note 前脚と頭。突進は «前» が来るので、光る面が進行方向を向く。
            Add(frontRightParts, glow); Add(frontLeftParts, glow);
            Add(headParts, glow);
            break;
        case BossAttackKind::Pulse:
            Add(bodyParts, glow);
            break;
        case BossAttackKind::Beam:
            /// @note コアは BossCoreComponent が極性色で握っている。
            ///       ここで琥珀を重ねると «どちらの極か» が読めなくなるので触らない。
            break;
        default:
            break;
        }
    }

    [[nodiscard]] const std::vector<Ref<GameObject>>& LegParts(BossLeg leg) const
    {
        switch (leg) {
        case BossLeg::FrontLeft:  return frontLeftParts;
        case BossLeg::BackRight:  return backRightParts;
        case BossLeg::BackLeft:   return backLeftParts;
        case BossLeg::FrontRight:
        default:                  return frontRightParts;
        }
    }

    /// 今フレームぶんを書き込み、前フレームで光っていて今は光らない部位を消す。
    /// @note 消す側を覚えておく。書いた部位だけを毎フレーム 0 に戻すと、
    ///       攻撃が切り替わった瞬間に前の部位が光ったまま焼き付く。
    void Flush()
    {
        const auto slot = static_cast<uint32_t>(materialSlot);

        for (const Lit& lit : m_lit) {
            const MaterialInstance instance = material.Instance(lit.ref, slot);
            if (!instance.IsValid() || !instance.HasProperty(kEmissiveScaleId)) continue;
            (void)instance.SetVector3(kEmissiveColorId,
                                      { warnColor.x, warnColor.y, warnColor.z });
            (void)instance.SetFloat(kEmissiveScaleId, lit.glow);
        }

        for (const EntityRef& previous : m_previous) {
            const bool stillLit = std::any_of(m_lit.begin(), m_lit.end(),
                [&](const Lit& lit) { return lit.ref == previous; });
            if (stillLit) continue;
            const MaterialInstance instance = material.Instance(previous, slot);
            if (!instance.IsValid() || !instance.HasProperty(kEmissiveScaleId)) continue;
            (void)instance.SetFloat(kEmissiveScaleId, 0.0f);
        }

        m_previous.clear();
        for (const Lit& lit : m_lit) m_previous.push_back(lit.ref);
    }

    [[nodiscard]] static const char* KindName(BossAttackKind kind)
    {
        switch (kind) {
        case BossAttackKind::Stomp:  return "Stomp";
        case BossAttackKind::Slam:   return "Slam";
        case BossAttackKind::Charge: return "Charge";
        case BossAttackKind::Beam:   return "Beam";
        case BossAttackKind::Pulse:  return "Pulse";
        case BossAttackKind::Erupt:  return "Erupt";
        case BossAttackKind::Sweep:  return "Sweep";
        default:                     return "None";
        }
    }

    BossAiComponent* m_ai = nullptr;
    BossAttackKind   m_kind = BossAttackKind::None;
    float            m_release = 0.0f;
    std::vector<Lit>       m_lit;
    std::vector<EntityRef> m_previous;
};

FBZZ_REFLECT(BossPartTelegraphComponent)

} // namespace sandbox
