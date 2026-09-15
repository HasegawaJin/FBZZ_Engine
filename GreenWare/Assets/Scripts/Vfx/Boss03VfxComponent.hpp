/// @file    Boss03VfxComponent.hpp
/// @brief   Boss03 の «体で言う» 予兆。翼の光り・自発光の脈・翼が落ちた後の煙
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// WHY 床だけでなく体でも言うか (BossPartTelegraphComponent と同じ理由):
///   このボスは頭上に浮いているので、近づくほど足元の床は本体で隠れる。
///   読ませたいのは «連撃の何拍目か» という時刻で、それは «見えているもの» =
///   ボスの体で言えなければ意味が無い。
///
/// 分割インポートされた翼メッシュは個別の発光で予告し、点光源で装甲の凹凸も拾う。
///
/// WHY 自発光も一緒に脈打たせるか:
///   取り込んだ .mat は emissiveScale が 0 で、発光用に分けてある材質
///   (B03_Emission_*) が完全に死んでいる。拍に合わせてここが与えると、
///   «今どの拍か» が体の芯からも読める。
#pragma once

#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Combat/Boss03AiComponent.hpp>
#include <Scripts/Combat/Boss03AnimParams.hpp>
#include <Scripts/Combat/Boss03AnimatorComponent.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cstdint>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class Boss03VfxComponent : public Script {
    FBZZ_SCRIPT(Boss03VfxComponent)

public:
    FBZZ_GROUP("翼の光り")
    FBZZ_FIELD_COLOR(warnColor, (Vector4{ 1.00f, 0.62f, 0.14f, 1.00f }), "Warn Color")
    FBZZ_TOOLTIP("危険の琥珀。DecalBossTelegraph の frameColor と揃えること")
    FBZZ_FIELD_RANGE(float, lightIntensity, 26.0f, "光の強さ", 0.0f, 200.0f)
    FBZZ_TOOLTIP("翼 1 枚ぶんの点光源。Point は «1m 地点での明るさ» なので、"
                 "屋外で存在感を出すなら 15〜30")
    FBZZ_FIELD_RANGE(float, lightRange, 6.0f, "光の範囲", 0.5f, 40.0f)
    FBZZ_FIELD_RANGE(float, lightOffset, 1.20f, "取り付け位置", 0.0f, 6.0f)
    FBZZ_TOOLTIP("骨の根元から翼に沿って (ローカル +Y) どれだけ先へ光を置くか [m]。"
                 "Boss03HitboxRigComponent の «翼の長さ» の半分が目安")

    FBZZ_GROUP("自発光")
    FBZZ_FIELD_RANGE_INT(int, coralSlot, 5, "Coral のスロット", -1, 15)
    FBZZ_TOOLTIP("B03_Emission_Coral の材質スロット。-1 で触らない")
    FBZZ_FIELD_RANGE_INT(int, coreSlot, 6, "Core のスロット", -1, 15)
    FBZZ_FIELD_RANGE_INT(int, indicatorSlot, 8, "Indicator のスロット", -1, 15)
    FBZZ_FIELD_RANGE(float, idleEmissive, 1.20f, "平常の明るさ", 0.0f, 20.0f)
    FBZZ_TOOLTIP("手を出していない間の自発光。0 だと «生きていない» 置物に見える")
    FBZZ_FIELD_RANGE(float, peakEmissive, 7.00f, "着弾直前の明るさ", 0.0f, 40.0f)
    FBZZ_TOOLTIP("Bloom のしきい値を越える値にしないと滲まない")
    FBZZ_FIELD_COLOR(idleColor, (Vector4{ 1.00f, 0.42f, 0.30f, 1.00f }), "平常の色")
    FBZZ_TOOLTIP("B03_Emission_Coral の色みに合わせる。予兆の間は琥珀へ寄る")

    // 拍。床のデカールと同じ言葉で刻む (BossTelegraphCue)。
    //
    // WHY 同じ値を持つか: 食い違うと足元と体が別々のリズムで光り、どちらで
    //     タイミングを取るのか決められなくなる。
    FBZZ_GROUP("拍")
    FBZZ_FIELD_RANGE_INT(int, pips, 3, "ピップの数", 0, 8)
    FBZZ_TOOLTIP("出す側が拍数を言ってきたらそちらが優先される。これは既定")
    FBZZ_FIELD_RANGE(float, pipGain, 2.1f, "ピップの明滅", 1.0f, 4.0f)
    FBZZ_FIELD_RANGE(float, strikeFrom, 0.82f, "打撃の起点", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, releaseSeconds, 0.12f, "解放", 0.0f, 1.0f)
    FBZZ_TOOLTIP("予兆が消えてから光が引くまで。0 だと着弾の瞬間に消えて «不発» に見える")

    // WHY 出し続けるか: 落ちた翼は消えずに残る (Detach レイヤーが畳んだ姿を保持する)。
    //   煙が 1 度で終わると «壊れた» が一瞬で流れ、盤面から «あと何枚» が読めない。
    FBZZ_GROUP("翼が落ちた後")
    FBZZ_FIELD(bool, smokeOnDetach, true, "断面から煙を出す")
    FBZZ_TOOLTIP("もいだ付け根から立ちのぼる煙。落ちている間ずっと出し続ける")
    FBZZ_FIELD_RANGE(float, smokeInterval, 0.35f, "煙の間隔", 0.05f, 3.0f)
    FBZZ_TOOLTIP("1 枚あたりの発生間隔 [秒]。短いほど濃いが、枚数ぶん積み上がる")
    FBZZ_FIELD_RANGE(float, smokeScale, 1.20f, "煙の大きさ", 0.1f, 4.0f)

    // WHY 撃破で光を落とすか: 自発光は «生きている» の表示でもある。倒した後も
    //   点いたまま崩れると «まだ動いている» に見えて、決着が絵から読めない。
    FBZZ_FIELD_RANGE(float, deathFadeSeconds, 1.60f, "撃破で光が落ちるまで", 0.0f, 8.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugKind, "None", "Kind")
    FBZZ_FIELD_READ_ONLY(float, debugGlow, 0.0f, "発光")
    FBZZ_FIELD_READ_ONLY(int, debugLights, 0, "光")

    void OnStart()  override;
    void OnUpdate() override;
    void OnDisable() override;
    void OnDestroy() override;

private:
    /// 翼 6 枚の骨へ点光源を吊る。骨は実行時に出来るので、揃うまで試し続ける。
    void EnsureLights();
    /// 今フレーム光らせる翼を決める。攻撃の種類と «どちらの側か» の対応表。
    [[nodiscard]] bool WingIsHot(int wing, BossAttackKind kind, int side) const;
    /// 自発光スロット 1 つへ書き込む。
    void DriveEmissive(int slot, const Vector4& color, float scale);
    BossGlowMaterials m_glowMaterials;
    BossArmorMaterials m_armorMaterials;
    /// 落ちた翼を見つけて、1 度だけ煙を出す。
    void WatchDetach();

    [[nodiscard]] Boss03AiComponent* Ai() const
    { return scene.GetScript<Boss03AiComponent>(); }
    [[nodiscard]] Boss03AnimatorComponent* Anim() const
    { return scene.GetScript<Boss03AnimatorComponent>(); }

    EntityRef m_lights[kBoss03WingCount];
    bool      m_smoked[kBoss03WingCount] = {};
    bool      m_wasToppled = false;
    float     m_probeCooldown = 0.0f;
    float     m_smokeTimer = 0.0f;
    float     m_release = 0.0f;
    /// 生きている度合い [0,1]。撃破でここが落ち、光の全部に掛かる。
    float     m_life = 1.0f;
    BossAttackKind   m_kind = BossAttackKind::None;
    BossTelegraphCue m_cue;

    static constexpr float kProbeInterval = 0.25f;
};

FBZZ_REFLECT(Boss03VfxComponent)


inline void Boss03VfxComponent::OnStart()
{
    m_glowMaterials.Reset(); m_armorMaterials.Reset();
    for (EntityRef& ref : m_lights) ref = {};
    for (bool& smoked : m_smoked) smoked = false;
    m_wasToppled    = false;
    m_probeCooldown = 0.0f;
    m_smokeTimer    = 0.0f;
    m_release       = 0.0f;
    m_life          = 1.0f;
    m_kind          = BossAttackKind::None;
    m_cue.Reset();

    if (!Ai())
        debug.LogError("Boss03VfxComponent requires Boss03AiComponent on the same object "
                       "(the attack progress is what decides what lights up).");
}

inline void Boss03VfxComponent::EnsureLights()
{
    // 全部揃っていれば探し直さない。
    bool missing = false;
    for (const EntityRef& ref : m_lights)
        if (!ref.Resolve(scene)) { missing = true; break; }
    if (!missing) return;

    m_probeCooldown -= std::max(Time::deltaTime, 0.0f);
    if (m_probeCooldown > 0.0f) return;
    m_probeCooldown = kProbeInterval;

    int built = 0;
    for (int i = 0; i < kBoss03WingCount; ++i) {
        if (m_lights[i].Resolve(scene)) { ++built; continue; }

        // 生成のたびに引き直す。scene.Create が GameObject 配列を再確保するので、
        // ループの外で掴んだポインタは 2 つ目以降で無効になる。
        GameObject* self = scene.Self();
        if (!self || !FindInSubtree(*self, kBoss03WingBones[i])) continue;

        GameObject& glow = scene.Create(std::string("FX_Glow_") + kBoss03WingBones[i]);
        // シーンには保存しない。Play のたびにその時点のリグへ吊り直す。
        glow.runtimeGenerated = true;

        GameObject* owner = scene.Self();
        if (GameObject* bone = owner ? FindInSubtree(*owner, kBoss03WingBones[i]) : nullptr)
            glow.SetParent(*bone);
        // 骨のローカル +Y が骨の向き。翼の途中へ置くと、根元と先の両方が明るくなる。
        glow.transform.position = { 0.0f, std::max(lightOffset, 0.0f), 0.0f };

        auto& light = glow.AddComponent<LightComponent>();
        light.type  = LightComponent::Type::Point;
        light.color = { warnColor.x, warnColor.y, warnColor.z };
        light.range = std::max(lightRange, 0.5f);
        light.intensity = 0.0f;
        // WHY 影を落とさないか: 6 枚ぶんの点光源に影を持たせると、1 枚ごとに
        //     影アトラスの面を取り合う。ここは «翼が光っている» を言うための光で、
        //     形を落とす役ではない。
        light.castShadows = false;
        glow.SetActive(false);

        m_lights[i] = EntityRef{ glow.GetID() };
        ++built;
    }
    debugLights = built;
}

inline void Boss03VfxComponent::OnDisable()
{
    for (const EntityRef& ref : m_lights)
        if (GameObject* object = ref.Resolve(scene)) object->SetActive(false);
    m_release = 0.0f;
    m_cue.Reset();
    DriveEmissive(coralSlot, idleColor, 0.0f);
    DriveEmissive(coreSlot, idleColor, 0.0f);
    DriveEmissive(indicatorSlot, idleColor, 0.0f);
}

inline void Boss03VfxComponent::OnDestroy()
{
    OnDisable();
    for (EntityRef& ref : m_lights) {
        if (GameObject* object = ref.Resolve(scene)) scene.Destroy(*object);
        ref = {};
    }
}

inline bool Boss03VfxComponent::WingIsHot(int wing, BossAttackKind kind, int side) const
{
    // 落ちた翼は光らない。
    if (const auto* anim = Anim(); anim && anim->IsWingDetached(wing)) return false;

    switch (kind) {
    case BossAttackKind::Slam: {
        // 連撃は «どちらの側の翼が来るか»。締め (side 0) は両側。
        if (side == 0) return true;
        const bool leftWing = wing < kBoss03WingCount / 2;
        return side < 0 ? leftWing : !leftWing;
    }
    case BossAttackKind::Sweep:
    case BossAttackKind::Pulse:
        // 突進と衝撃波は体ごと来る。全部光らせて «避ける向きが無い» を言う。
        return true;
    default:
        // 焼き払いは線そのものが予兆なので、翼は光らせない
        // (Boss03AiComponent の TickLaserWindup の WHY)。
        return false;
    }
}

inline void Boss03VfxComponent::DriveEmissive(int slot, const Vector4& color, float scale)
{
    if (slot < 0) return;
    const char* filter = slot == coreSlot ? "Emission_Core"
                       : slot == indicatorSlot ? "Emission_Indicator" : "Emission_Coral";
    m_glowMaterials.Apply(scene.Self(), material, color, scale, Time::time, filter);
}

inline void Boss03VfxComponent::WatchDetach()
{
    const auto* anim = Anim();
    if (!anim) return;

    // 落ちた瞬間の後始末。光は残すと «無い翼が光っている» になる。
    for (int i = 0; i < kBoss03WingCount; ++i) {
        if (m_smoked[i] || !anim->IsWingDetached(i)) continue;
        m_smoked[i] = true;
        if (GameObject* glow = m_lights[i].Resolve(scene)) glow->SetActive(false);
    }

    if (!smokeOnDetach || anim->IsDead()) return;

    m_smokeTimer -= std::max(Time::deltaTime, 0.0f);
    if (m_smokeTimer > 0.0f) return;
    m_smokeTimer = std::max(smokeInterval, 0.05f);

    auto* vfx = VfxManagerComponent::Instance();
    GameObject* self = scene.Self();
    if (!vfx || !self) return;

    const Vector3 body = transform.worldPosition;
    for (int i = 0; i < kBoss03WingCount; ++i) {
        if (!anim->IsWingDetached(i)) continue;
        GameObject* bone = FindInSubtree(*self, kBoss03WingBones[i]);
        if (!bone) continue;

        // 断面は付け根。流れる向きは «体から外へ» の水平成分 ─ 上向きは煙の側が持つ。
        const Vector3 at = bone->transform.worldPosition;
        Vector3 drift = at - body;
        drift.y = 0.0f;
        vfx->PlayLegSmoke(at, drift.NormalizedOr(Vector3::RIGHT), 1.0f,
                          std::max(smokeScale, 0.1f));
    }
}

inline void Boss03VfxComponent::OnUpdate()
{
    const auto* ai = Ai();
    if (!ai) return;

    EnsureLights();
    WatchDetach();

    const BossTelegraph& telegraph = ai->CurrentTelegraph();
    const bool active = telegraph.shape != BossTelegraphShape::None
                     && telegraph.kind  != BossAttackKind::None;

    // 予兆が消えた後も少しだけ残す。着弾と同時に消えると «出なかった» に見える。
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

    m_cue.pips         = telegraph.pips > 0 ? telegraph.pips : std::max(pips, 0);
    m_cue.pipGain      = pipGain;
    m_cue.strikeFrom   = Clamp01(strikeFrom);
    m_cue.burstSeconds = 0.0f;   // 消え際は releaseSeconds が持っている
    m_cue.Tick(progress, std::max(Time::deltaTime, 0.0f), active);

    // 撃破したら光が落ちていく。爆ぜるのは BossDeathVfxComponent の担当で、
    // こちらは «消えていく» 側だけを持つ。
    const auto* anim = Anim();
    const bool  dead = anim && anim->IsDead();
    if (!dead) m_armorMaterials.Apply(scene.Self(), material, envelope * envelope,
        ai->IsToppled(), ai->BodyHitFlash(), Time::deltaTime);
    if (dead && deathFadeSeconds > 0.0f)
        m_life = std::max(m_life - Time::deltaTime / deathFadeSeconds, 0.0f);
    else if (dead)
        m_life = 0.0f;

    // 進みの二乗で立ち上げる。線形だと予兆の前半から明るく、«あと少し» が出ない。
    float glow = envelope * envelope;
    // 回避窓では «脈» をやめて張り付かせる。明滅が止まることが «今» の合図になる。
    if (active) glow *= m_cue.pulse * Lerp(1.0f, 1.35f, m_cue.strike);
    glow = std::max(glow, 0.0f) * m_life;

    debugKind = active ? "Telegraph" : (m_release > 0.0f ? "Release" : "None");
    debugGlow = glow;

    const int side = ai->SwingSide();
    for (int i = 0; i < kBoss03WingCount; ++i) {
        GameObject* object = m_lights[i].Resolve(scene);
        if (!object) continue;
        const float lit = (!anim || !anim->IsWingDetached(i)) && WingIsHot(i, m_kind, side) ? glow : 0.0f;
        // 0 の光を回し続けると、点いていない光源ぶんのクラスタ割り当てだけが残る。
        if (lit <= 0.0f) { if (object->activeSelf()) object->SetActive(false); continue; }

        if (!object->activeSelf()) object->SetActive(true);
        if (auto* light = object->GetComponent<LightComponent>()) {
            light->color     = { warnColor.x, warnColor.y, warnColor.z };
            light->range     = std::max(lightRange, 0.5f);
            light->intensity = lit * std::max(lightIntensity, 0.0f);
        }
    }

    // 自発光。平常はコーラルで点り、予兆の間だけ琥珀へ寄って脈を打つ。
    const float blend = Clamp01(glow);
    const Vector4 tint{ Lerp(idleColor.x, warnColor.x, blend),
                        Lerp(idleColor.y, warnColor.y, blend),
                        Lerp(idleColor.z, warnColor.z, blend), 1.0f };
    const float scale = Lerp(std::max(idleEmissive, 0.0f), std::max(peakEmissive, 0.0f), blend)
                      * m_life;
    DriveEmissive(coralSlot,     tint, scale);
    DriveEmissive(indicatorSlot, tint, scale);
    m_glowMaterials.Apply(scene.Self(), material, tint, scale, Time::time,
                          "Emission", "Boss03_Body", 0.0f);
    // 芯は脈を浅くする。ここまで一緒に明滅すると、体ごと点滅しているように見える。
    DriveEmissive(coreSlot, tint, Lerp(std::max(idleEmissive, 0.0f),
                                       std::max(peakEmissive, 0.0f), blend * 0.5f) * m_life);

    // 動かない翼まで警告色に染めると左右の予告が読めなくなる。
    if (!dead) {
        const bool opening = ai->IsToppled();
        const float openingPulse = 0.5f + 0.5f * std::sin(Time::time * 4.0f);
        for (int i = 0; i < kBoss03WingCount; ++i) {
            if (anim && anim->IsWingDetached(i)) continue;
            const bool hot = WingIsHot(i, m_kind, side) && envelope > 0.0f;
            const Vector4 wingTint = opening ? Vector4{1.0f, 0.78f, 0.26f, 1.0f}
                                            : hot ? tint : idleColor;
            const float wingScale = opening ? 1.4f + 0.5f * openingPulse
                                           : hot ? scale : std::max(idleEmissive, 0.0f);
            const std::string meshName = std::string("Boss03_") + kBoss03WingBones[i];
            m_glowMaterials.Apply(scene.Self(), material, wingTint, wingScale, Time::time,
                                  "Emission", meshName.c_str(), hot ? 0.0f : 0.12f);
        }
        if (opening) {
            const Vector4 exposedColor{1.0f, 0.78f, 0.24f, 1.0f};
            DriveEmissive(coreSlot, exposedColor, 2.4f + openingPulse * 0.8f);
            m_glowMaterials.Apply(scene.Self(), material, exposedColor, 1.6f + openingPulse * 0.5f,
                                  Time::time, "Emission", "Boss03_Body", 0.0f);
        }
    }

    if (!dead && !active && ai->BodyHitFlash() > 0.0f) {
        const float hit = Clamp01(ai->BodyHitFlash());
        const Vector4 hitTint{1.0f, Lerp(idleColor.y, 0.94f, hit),
                            Lerp(idleColor.z, 0.78f, hit), 1.0f};
        m_glowMaterials.Apply(scene.Self(), material, hitTint, idleEmissive + 2.4f * hit,
                              Time::time, "Emission", "Boss03_Body", 0.0f);
        m_glowMaterials.Apply(scene.Self(), material, hitTint, idleEmissive + 1.6f * hit,
                              Time::time, "Emission", "Boss03_Core", 0.0f);
    }

    // 崩れた瞬間。倒れている «出来事» は床側で 1 度だけ出す。
    const bool toppled = ai->IsToppled();
    if (toppled && !m_wasToppled)
        if (auto* vfx = VfxManagerComponent::Instance())
            vfx->PlayTopple(transform.worldPosition, 1.2f);
    m_wasToppled = toppled;
}

} // namespace sandbox
