/// @file    KillCoreComponent.hpp
/// @brief   撃破された敵がその場に残す一時アンカー。数秒だけ «的» として使える
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// WHY 要るか (Docs/polarity-system.md「撃破コア」):
///   壁や地形には極性を付与できず、極を持てるのは敵だけになっている。それだけだと
///   敵が 1 体になった瞬間に手詰まりになる。倒すたびに «次の一手の的» が生まれれば、
///   最後の 1 体も直前に倒した相手のコアへ叩きつけて処理できる。
///   同時に、多対 1 の集束が成立する数少ないアンカーでもある。動く敵どうしは
///   リンクを 1 本しか張れないので、これが無いと大きい集束はほとんど発火しない。
///
/// WHY 独立した GameObject にするか (死体を居残らせるのではなく):
///   撃破演出 (EnemyDeathVfxComponent) は体をディゾルブで «食って» 消す。0.8 秒で
///   面が無くなるので、死体をそのまま的にすると «見えない的» になる。
///   コアは死体とは別の寿命を持つ別の物として置く。
///
/// WHY 見た目を粒 1 種類で作るか:
///   コアはメッシュを持たない。板を 1 枚置くとカメラ角度で潰れ、床の起伏に刺さる。
///   同じ点から出続ける粒をビルボードで重ねれば、どこから見ても同じ塊に見えて、
///   «脈打っている» も寿命のグラデーションだけで出る。絵の中身は PolarityCore.hlsl。
///
/// WHY selfDriven にするか:
///   PolarityTargetComponent は既定で «発光を書き込める Material» を要求する。
///   コアは粒だけで出来ていてメッシュを持たないので、その要求を満たせない。
///   selfDriven は «極も見た目も持ち主が決める» という宣言で、まさにこの構成を指す。
///   acceptsPaint を立てて、外から極を乗せ直すことだけは許す (残り時間も進む)。
///
/// WHY ＋と−で .mat を差し替えるか:
///   ParticlePass は材質を materialPath をキーにグローバルへ 1 つだけ持ち、
///   GameObject ごとの paramOverrides はパーティクルへ届かない。極ごとに «違う素材»
///   を持つのが唯一の道になる (PolarityCore.hlsl のヘッダー)。
#pragma once

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class KillCoreComponent : public Script {
    FBZZ_SCRIPT(KillCoreComponent)

public:
    // 生成側 (CombatManagerComponent) が注入する。
    fbzz::Asset<PolarityTuning> tuning{};

    FBZZ_GROUP("Material")
    FBZZ_FIELD_FILE(materialPlus, "Assets/Materials/Effects/FX_POL_Core_Plus.mat",
                    "Core (+)", ".mat")
    FBZZ_FIELD_FILE(materialMinus, "Assets/Materials/Effects/FX_POL_Core_Minus.mat",
                    "Core (-)", ".mat")
    FBZZ_TOOLTIP("極ごとに別の .mat が要る。パーティクルには GameObject 単位の "
                 "override が届かないため")

    FBZZ_GROUP("Look")
    FBZZ_FIELD_RANGE(float, coreSize, 1.15f, "Size", 0.1f, 6.0f)
    FBZZ_TOOLTIP("塊の直径 [m]。敵より一回り小さくして «残骸» に見せる")
    FBZZ_FIELD_RANGE(float, pulseAmount, 0.35f, "Pulse", 0.0f, 2.0f)
    FBZZ_TOOLTIP("古い粒がどれだけ膨らむか。0 で脈打たない硬い球になる")
    FBZZ_FIELD_RANGE(float, pulseSeconds, 0.85f, "Pulse Seconds", 0.1f, 4.0f)
    FBZZ_TOOLTIP("粒 1 つの寿命。これが脈の周期になる")
    FBZZ_FIELD_RANGE(float, density, 6.0f, "Density", 1.0f, 40.0f)
    FBZZ_TOOLTIP("毎秒の発生数。寿命と掛けた数だけ常時重なる。"
                 "少ないと «明滅» に見え、多いと加算で白く飽和する")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugRemaining, 0.0f, "Remaining")
    FBZZ_FIELD_READ_ONLY(std::string, debugMaterial, "", "Material")

    /// 生成直後に 1 度だけ呼ぶ。極を乗せ、寿命を始める。
    ///
    /// WHY OnStart ではなく外から渡すか: どの極で残すかは «誰が誰を倒したか» の
    ///     結果で、コア自身は知らない。生成した側だけが知っている。
    void Configure(Polarity polarity);

    void OnStart()  override;
    void OnUpdate() override;
    /// 枠を畳むときに粒も止める。止めないと、無効化された枠から粒が出続ける。
    void OnDisable() override { particle.Stop(/*clear=*/true); }

private:
    /// 極を実際に乗せる。乗ったら true。
    bool ApplyPolarity(Polarity polarity);
    /// 今の極に合わせてエミッターを組み直す。
    void ConfigureEmitter(Polarity polarity);
    [[nodiscard]] PolarityTargetComponent* Target() const
    {
        return scene.GetScript<PolarityTargetComponent>();
    }

    /// 乗せようとしている極。乗り切るまで覚えておく。
    Polarity m_wanted = Polarity::None;
    /// 乗り直しを許す残りフレーム数。
    ///
    /// WHY 要るか: 枠は非アクティブで寝かせてある。非アクティブな GameObject の
    ///     Script は OnStart が走らないので、起こした «後» に
    ///     PolarityTargetComponent::OnStart が回り、その中の ClearPolarity() が
    ///     こちらで乗せたばかりの極を消す。生まれた次のフレームに «切れた» と
    ///     判断して自分で畳んでしまうため、数フレームだけ乗せ直しを許す。
    int m_settleFrames = 0;
};

FBZZ_REFLECT(KillCoreComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void KillCoreComponent::OnStart()
{
    // 生まれる «場所» で鳴らすので 3D。出どころが無いと PlayAt は黙って捨てられる。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void KillCoreComponent::Configure(Polarity polarity)
{
    if (polarity == Polarity::None) return;

    // WHY ここでも音源を用意するか: 枠は非アクティブで寝かせてあるので、この
    //     Configure が走る時点ではまだ OnStart を抜けていないことがある
    //     (非アクティブな GameObject の Script は OnStart が呼ばれない)。
    //     EnsureSource は既にあれば何もしないので、両方から呼んで構わない。
    se::EnsureSource(scene, "SE", 1.0f);

    m_wanted       = polarity;
    m_settleFrames = 4;

    if (!ApplyPolarity(polarity)) return;

    // 生まれた «場所» で鳴らす。倒した相手の位置に的が残ったことが、
    // 画面の別の場所を見ていても方向で分かる。
    se::PlayAt(audio, se::kPolarityInfect, transform.worldPosition);
}

inline bool KillCoreComponent::ApplyPolarity(Polarity polarity)
{
    // WHY 先に確かめるか: 極を乗せる経路は必ず tuning を読む (持続時間・硬直)。
    //     生成側が盤面を見つけられずに渡し損ねていると、最初の撃破でそこを踏む。
    if (!tuning) {
        debug.LogError("KillCoreComponent has no PolarityTuning. The spawner "
                       "(CombatManagerComponent) could not find PolarityFieldComponent.");
        return false;
    }

    auto* target = Target();
    if (!target) {
        // 生成側が付け忘れている。付いていないと盤面の候補に入らず、
        // «倒したのに的が出ない» が無言で起きる。
        debug.LogError("KillCoreComponent requires PolarityTargetComponent on the same "
                       "object. The core will never be a target for convergence.");
        return false;
    }

    target->tuning.ref     = tuning.ref;
    target->polarityClass  = PolarityClass::Pillar;   // 撃破コアの寿命 (durationPillar)
    target->isAnchor       = true;                    // 引力では動かない。的になる側
    target->selfDriven     = true;                    // メッシュを持たないので見た目は自前
    target->acceptsPaint   = true;                    // 極を乗せ直すことは許す
    target->outlineEnabled = false;                   // 輪郭を描く形が無い
    // 枠を作った時点で盤面がまだ居らず、調整値を渡せずに自分を止めていることがある
    // (PolarityTargetComponent::OnStart)。ここでは必ず揃っているので起こし直す。
    target->enabled        = true;

    // 枠は使い回す。前の極が残っていると、逆の極で生まれ直したときに Apply が
    // «中和» と解釈して無極になり、生まれた瞬間に自分で畳んでしまう。
    target->ClearPolarity();

    // Apply を通すのは、残り時間の起点 (durationPillar) を 1 箇所に保つため。
    // SetPolarity は selfDriven のボス用で、残り時間を持たない。
    (void)target->Apply(polarity);

    ConfigureEmitter(polarity);
    return target->IsCharged();
}

inline void KillCoreComponent::ConfigureEmitter(Polarity polarity)
{
    // シーンで作り込んだエミッターがあればそれを使い、無ければ足す。
    ParticleEmitter& emitter = scene.GetOrAddComponent<ParticleEmitter>();
    ParticleEmitterSettings& s = emitter.settings;

    s.materialPath = polarity == Polarity::Plus ? materialPlus : materialMinus;
    debugMaterial  = s.materialPath;

    // 1 点から出し続ける。散らすと «塊» ではなく «雲» になり、的として狙えない。
    s.shape           = ParticleEmitterShape::Point;
    s.emitPosition    = Vector3::ZERO;
    s.emitVelocity    = Vector3::ZERO;
    s.velocitySpread  = 0.0f;
    s.velocityDamping = 0.0f;
    s.gravity         = Vector3::ZERO;
    s.noiseStrength   = 0.0f;
    // WHY Local か: コアは動かないが、枠は使い回して別の場所で生まれ直す。World だと
    //     前の位置に出ていた粒が寿命ぶん «置き去り» で残り、居ない的が光って見える。
    s.simulationSpace = ParticleSimulationSpace::Local;

    // 寿命がそのまま脈の周期。古いほど膨らんで薄れる ＝ 呼吸しているように見える。
    s.lifetime       = std::max(pulseSeconds, 0.1f);
    s.lifetimeRandom = 0.0f;
    s.sizeStart      = std::max(coreSize, 0.05f);
    s.sizeEnd        = s.sizeStart * (1.0f + std::max(pulseAmount, 0.0f));
    // 色は白のまま。極の色は .mat が持つ (パーティクルへ override が届かないため)。
    // ここで動かすのは «若さ» だけで、シェーダーはそれを alpha から読む。
    s.colorStart     = { 1.0f, 1.0f, 1.0f, 1.0f };
    s.colorEnd       = { 1.0f, 1.0f, 1.0f, 0.0f };
    s.colorVariation = 0.0f;

    s.emitRate = std::max(density, 1.0f);
    // 寿命ぶん重なる数に少し余裕を持たせる。足りないと発生が間引かれて脈が飛ぶ。
    s.maxParticles = std::max(static_cast<int>(s.emitRate * s.lifetime) + 8, 8);

    // 全部が同じ位置・同じ大きさなので前後関係が絵に出ない。並べ替えは要らない。
    s.sortMode = ParticleSortMode::None;
    s.loop     = true;
    s.duration = 0.0f;
    s.playing  = true;
    s.enabled  = true;

    particle.Play(/*restart=*/true);
}

inline void KillCoreComponent::OnUpdate()
{
    auto* target = Target();
    if (!target) return;

    debugRemaining = target->RemainingSeconds();

    // 生まれ立ての数フレームは «乗り切るまで» 掛け直す。寝ていた枠を起こした直後は
    // PolarityTargetComponent の OnStart がこの後に回り、乗せたばかりの極を消す。
    if (m_settleFrames > 0) {
        --m_settleFrames;
        if (!target->IsCharged()) {
            (void)ApplyPolarity(m_wanted);
            return;   // 寿命の判定はまだしない
        }
        m_settleFrames = 0;
        m_wanted       = Polarity::None;
    }

    // 極が切れた = 寿命が尽きた。枠をプールへ返す。
    //
    // WHY 破棄せず畳むか: コアは倒すたびに生まれる。毎回 GameObject を作って捨てると、
    //     シーンの GameObject 数が戦闘の激しさに合わせて上下し、EntityID を握っている
    //     側の参照が揺さぶられる (ビームと VFX の枠が同じ理由で寝かせてある)。
    if (!target->IsCharged()) {
        // 粒ごと消してから畳む。残すと、次に別の場所で使い回したときに
        // 前の位置の粒が 1 フレームだけ見える。
        particle.Stop(/*clear=*/true);
        if (GameObject* self = scene.Self()) self->SetActive(false);
        return;
    }

    // 塗り直しで極が変わったら素材を差し替える。差し替えないと、色だけ前の極のまま
    // «赤い − のコア» になり、盤面の記号が嘘になる。
    const std::string& wantedMaterial = target->Current() == Polarity::Plus ? materialPlus
                                                                            : materialMinus;
    if (debugMaterial != wantedMaterial) ConfigureEmitter(target->Current());
}

} // namespace sandbox
