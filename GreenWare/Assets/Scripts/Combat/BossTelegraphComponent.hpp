/// @file    BossTelegraphComponent.hpp
/// @brief   BossAiComponent が出す予兆を、地面のデカール 1 枚として描く
/// @author  Hasegawa Jin
/// @date    2026-08-30
///
/// WHY 板ではなくデカールか:
///   板を 1 枚置くと、床の起伏・瓦礫・ボスの足元でめり込んで切れる。デカールは
///   深度から受け面を復元して投影するので、範囲が地形に沿って «敷かれた» 形になる
///   (DecalPolarityRing と同じ理由)。
///
/// WHY 1 枚だけ持つか:
///   ボスは 1 度に 1 つの行動しか出さない (Act は排他)。枚数を増やしても同時に
///   2 つ出る経路が無く、使われない枠の «消し忘れ» を疑う手間だけが増える。
///
/// WHY AI に形を作らせて、こちらは描くだけにするか:
///   着弾点も射程も攻撃の進行が持つ値。ここで同じ計算を持つと、AI の数値を触るたびに
///   «予兆だけ古い場所に出る» が起きる (BossTelegraph.hpp の WHY)。
#pragma once

#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Combat/DangerWallComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossTelegraphComponent : public Script {
    FBZZ_SCRIPT(BossTelegraphComponent)

public:
    FBZZ_GROUP("Decal")
    FBZZ_FIELD_FILE(telegraphMaterial, "Assets/Materials/Decal/DecalBossTelegraph.mat",
                    "Material", ".mat")
    FBZZ_FIELD_RANGE(float, projectionDepth, 6.0f, "Depth", 0.2f, 30.0f)
    FBZZ_TOOLTIP("投影の厚み [m]。薄いと坂で切れ、厚いと段差の裏にも回り込む")

    FBZZ_GROUP("Feel")
    // WHY 出だしを薄くするか: 予兆が最初から満濃度で出ると «もう来た» に見える。
    //     立ち上がりに一拍あると «来る» と «来た» が分かれる。
    FBZZ_FIELD_RANGE(float, fadeInSeconds, 0.12f, "Fade In", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, blinkHz, 0.0f, "Blink Hz", 0.0f, 20.0f)
    FBZZ_TOOLTIP("着弾間際の明滅。0 で明滅なし。進みが Blink From を超えてから掛かる")
    FBZZ_FIELD_RANGE(float, blinkFrom, 0.70f, "Blink From", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, blinkDepth, 0.35f, "Blink Depth", 0.0f, 1.0f)

    // WHY 模様を流すか: 止まった斜線は «床に描いてある柄» と区別が付かない。
    //     流れていると «今それが起きつつある» になり、帯では «どちらから来るか»
    //     まで同じ模様が言う。デカールの cbuffer に時刻が無いので、位相は
    //     こちらが毎フレーム進めて渡す (DecalPolarityRing の spin と同じ形)。
    FBZZ_FIELD_RANGE(float, stripeScrollHz, 0.45f, "Stripe Scroll", -4.0f, 4.0f)
    FBZZ_TOOLTIP("斜線と矢羽根が流れる速さ [周/秒]。帯では正で «ボスから前へ» 流れる。"
                 "0 で止まる")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugShape, "None", "Shape")
    FBZZ_FIELD_READ_ONLY(float, debugProgress, 0.0f, "Progress")

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    [[nodiscard]] std::string DecalName() const;
    /// デカールを拾い直す。無ければ作る。
    [[nodiscard]] GameObject* EnsureDecal();
    void Hide(GameObject& object);
    void Place(GameObject& object, const BossTelegraph& telegraph);

    EntityRef m_decal;
    /// 予兆が出てからの秒数。立ち上がりのフェードに使う。
    float m_shownFor = 0.0f;
};

FBZZ_REFLECT(BossTelegraphComponent)

inline std::string BossTelegraphComponent::DecalName() const
{
    GameObject* owner = scene.Self();
    return "BossTelegraph_" + (owner ? owner->instanceId : std::string("orphan"));
}

inline GameObject* BossTelegraphComponent::EnsureDecal()
{
    // WHY 先に拾い直すか: スクリプト DLL をリロードすると Script は作り直され
    //     EntityRef は空に戻るが、デカールの GameObject は Scene に残る。
    //     無条件に作るとリロードのたびに 1 枚ずつ増えていく。
    if (GameObject* existing = m_decal.Resolve(scene)) return existing;

    GameObject* object = scene.Find(DecalName());
    if (!object) {
        // WHY ボスの子にしないか: 子にすると位置と «向き» がボスの回転を引き継ぐ。
        //     予兆はワールドの形なので、ボスが旋回しただけで帯が振り回される。
        GameObject& created = scene.Create(DecalName());
        created.runtimeGenerated = true;
        object = &created;
    }

    DecalComponent* decal = object->GetComponent<DecalComponent>();
    if (!decal) decal = &object->AddComponent<DecalComponent>();
    if (!telegraphMaterial.empty()) decal->materialPath = telegraphMaterial;

    m_decal = EntityRef{ object->GetID() };
    return object;
}

inline void BossTelegraphComponent::Hide(GameObject& object)
{
    object.SetActive(false);
    m_shownFor    = 0.0f;
    debugShape    = "None";
    debugProgress = 0.0f;
}

inline void BossTelegraphComponent::Place(GameObject& object, const BossTelegraph& telegraph)
{
    auto* decal = object.GetComponent<DecalComponent>();
    if (!decal) return;

    const bool  line     = telegraph.shape == BossTelegraphShape::Line;
    const float depth    = std::max(projectionDepth, 0.2f);
    const float progress = Clamp01(telegraph.progress);

    // デカールの投影軸はローカル +Y。
    //
    // 円 … 直径 × 直径の箱。回転は要らない
    // 帯 … 幅 (半幅の 2 倍) × 長さの箱。始点ではなく «中点» へ置く
    if (line) {
        const Vector3 dir = telegraph.direction.NormalizedOr(Vector3::FORWARD);
        const Vector3 mid = telegraph.origin + dir * (telegraph.length * 0.5f);
        object.transform.position      = mid;
        object.transform.worldPosition = mid;
        // +Z を進行方向へ向ける。投影軸 (+Y) は起こしたまま。
        object.transform.rotation =
            Quaternion::FromAxisAngle(Vector3::UP, std::atan2(dir.x, dir.z));
        object.transform.scale = { std::max(telegraph.radius, 0.05f) * 2.0f,
                                   depth,
                                   std::max(telegraph.length, 0.1f) };
    } else {
        object.transform.position      = telegraph.origin;
        object.transform.worldPosition = telegraph.origin;
        object.transform.rotation      = Quaternion::Identity();
        const float diameter = std::max(telegraph.radius, 0.05f) * 2.0f;
        object.transform.scale = { diameter, depth, diameter };
    }

    // 立ち上がりの薄さと、着弾間際の明滅。
    //
    // WHY 明滅を «終わり際» に限るか: 最初から点滅していると、予兆が出ている間ずっと
    //     «今すぐ来る» と言い続けることになり、いつ避けるかが読めない。
    float gain = fadeInSeconds > 0.0f ? Clamp01(m_shownFor / fadeInSeconds) : 1.0f;
    if (blinkHz > 0.0f && progress >= Clamp01(blinkFrom)) {
        const float wave = std::sin(Time::time * blinkHz * TWO_PI) * 0.5f + 0.5f;
        gain *= Lerp(1.0f - Clamp01(blinkDepth), 1.0f, wave);
    }

    decal->materialParamOverrides["shape"]    = { line ? 1.0f : 0.0f };
    decal->materialParamOverrides["progress"] = { progress };
    // «上から来る» は落下リングで言う。形 (円 / 帯) と直交する軸なので、
    // 攻撃の種類から引く (BossTelegraph.hpp の BossThreatOriginOf)。
    decal->materialParamOverrides["threatAbove"] =
        { BossThreatOriginOf(telegraph.kind) == BossThreatOrigin::Above ? 1.0f : 0.0f };
    decal->materialParamOverrides["pulse"]    = { std::max(gain, 0.0f) };
    // 位相は «出てからの経過» で進める。全体時計を渡すと、予兆が出た瞬間の模様の
    // 位置が毎回違って «同じ攻撃が同じ絵で来る» が崩れる。
    decal->materialParamOverrides["stripeScroll"] = { m_shownFor * stripeScrollHz };

    debugShape    = line ? "Line" : "Circle";
    debugProgress = progress;

    // 帯の予兆は «立てて» も見せる。ボスの体で床が隠れる距離ほど、
    // 通り道の高さが読めないと避けようがない (DangerWallComponent の WHY)。
    if (auto* wall = scene.GetScript<DangerWallComponent>()) wall->Submit(telegraph);
}

inline void BossTelegraphComponent::OnStart()
{
    if (!scene.GetScript<BossAiComponent>()) {
        debug.LogError("BossTelegraphComponent requires BossAiComponent on the same object "
                       "(the attack progress is what decides the telegraph).");
        enabled = false;
        return;
    }

    if (GameObject* object = EnsureDecal()) Hide(*object);
}

inline void BossTelegraphComponent::OnLateUpdate()
{
    const auto* ai = scene.GetScript<BossAiComponent>();
    GameObject* object = EnsureDecal();
    if (!ai || !object) return;

    const BossTelegraph& telegraph = ai->CurrentTelegraph();
    if (telegraph.shape == BossTelegraphShape::None) {
        if (object->activeSelf()) Hide(*object);
        return;
    }

    if (!object->activeSelf()) {
        object->SetActive(true);
        m_shownFor = 0.0f;
    }
    m_shownFor += std::max(Time::deltaTime, 0.0f);

    Place(*object, telegraph);
}

inline void BossTelegraphComponent::OnDestroy()
{
    // ルートに置いた以上、ボスと一緒には消えない。持ち主が畳む。
    if (GameObject* object = m_decal.Resolve(scene))
        scene.Destroy(*object);
    m_decal = {};
}

} // namespace sandbox
