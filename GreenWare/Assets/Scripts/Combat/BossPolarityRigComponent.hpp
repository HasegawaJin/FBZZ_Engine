/// @file    BossPolarityRigComponent.hpp
/// @brief   ボスの部位に乗った極どうしを引き合わせ、成立したらボス自身を転倒させる
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY 部位を «実際に» 引き寄せないか:
///   脚はスキンドメッシュのボーンで、AnimatorSystem が Phase::LateUpdate に位置も回転も
///   毎フレーム書き直す。Script から動かしても消えるため、引き合いを絵にするには
///   IK か専用クリップが要る。ここでは «引き合っている» を溜めの演出で見せ、
///   結果だけを既存の転倒 (Crash) へ落とす。芯が面白いかは結果の方で決まる。
///
/// WHY 転倒を BossAiComponent の激突スタンへ流すか:
///   «5 秒無防備・コア消灯・Boss_Crash モーション» は突進を壁へ誘導したとき用に
///   既に作ってある。転倒に別の状態を足すと、同じ «倒れている» が 2 系統になり、
///   復帰処理 (照射の後始末・重力・硬直の解除) を 2 箇所で持つことになる。
#pragma once

#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPartPolarityComponent.hpp>
#include <Scripts/Combat/BossPolarityCoreComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Polarity/PolarityRingComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossPolarityRigComponent : public Script {
    FBZZ_SCRIPT(BossPolarityRigComponent)

public:
    FBZZ_GROUP("Pull")
    FBZZ_FIELD_RANGE(float, pullSeconds, 0.90f, "Windup", 0.1f, 5.0f)
    FBZZ_TOOLTIP("異極の 2 部位が揃ってから転倒するまで。ここが «見せ場» の長さで、"
                 "短いと «斬った瞬間に勝手に転んだ» になり、長いと待たされる")
    // WHY 対の距離を測るか: 部位はどれもボスの体の中にあるので既定では必ず届く。
    //     «離れた 2 本ほど大きく崩れる» を後から入れるための入口として持つ。
    FBZZ_FIELD_RANGE(float, pairRadius, 14.0f, "Pair Radius", 1.0f, 40.0f)
    FBZZ_TOOLTIP("対として成立する部位間の距離。ボスの全幅は 9m なので既定では全対が届く")
    FBZZ_FIELD_RANGE(float, pullDecay, 2.5f, "Decay", 0.5f, 20.0f)
    FBZZ_TOOLTIP("対が崩れたとき溜めが戻る速さ。実時間の倍率")

    FBZZ_GROUP("Topple")
    FBZZ_FIELD_RANGE(float, toppleSeconds, 5.0f, "Topple Seconds", 0.5f, 15.0f)
    FBZZ_TOOLTIP("転倒して無防備な秒数。既定は激突スタンと同じ 5 秒")
    FBZZ_FIELD_RANGE_INT(int, toppleSelfDamage, 40, "Self Damage", 0, 2000)
    FBZZ_TOOLTIP("転倒そのもので入るダメージ。0 にすると «隙を作るだけ» になる")

    FBZZ_GROUP("Feel")
    // WHY 帯電中の部位を毎フレーム描くか: 部位の当たり判定はレンダラーを持たないので、
    //     極が乗っても脚そのものの見た目は 1 ピクセルも変わらない。«どの脚に何が
    //     乗っているか» が見えないと、2 本目をどこへ入れるかという判断が成立しない。
    //     本番では脚の装甲を光らせる (メッシュの分割が要る) が、それは芯が面白いと
    //     決まってからで間に合う。
    FBZZ_FIELD(bool, drawPartMarks, true, "Draw Part Marks")
    FBZZ_TOOLTIP("帯電している部位を極の色で囲う。脚を光らせるまでの仮表示")
    FBZZ_FIELD_RANGE(float, markRadius, 1.00f, "Mark Radius", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, ringIntervalFar, 0.32f, "Ring Interval (start)", 0.02f, 2.0f)
    FBZZ_FIELD_RANGE(float, ringIntervalNear, 0.07f, "Ring Interval (end)", 0.02f, 2.0f)
    FBZZ_TOOLTIP("引き合いが始まってからの環の間隔。詰まっていくことで «来る» が耳と目で読める")
    FBZZ_FIELD_RANGE(float, ringRadius, 2.2f, "Ring Radius", 0.2f, 10.0f)
    FBZZ_FIELD_RANGE(float, toppleHitStop, 0.40f, "Hitstop", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, toppleShake, 0.85f, "Shake", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, toppleFlash, 0.55f, "Flash", 0.0f, 1.0f)
    FBZZ_FIELD(bool, drawPullLink, true, "Draw Link")
    FBZZ_TOOLTIP("引き合っている 2 部位を線で結ぶ。帯 (BeamTrail) を入れるまでの仮表示")

    // 引き合いを «脚が寄る» 絵にする。ボーンは AnimatorSystem が LateUpdate で毎フレーム
    // 書き直すので、IKSystem (同じ LateUpdate の後段) から動かすのが唯一の経路になる。
    FBZZ_GROUP("Leg IK")
    FBZZ_FIELD(bool, pullLegs, true, "Pull Legs")
    FBZZ_TOOLTIP("引き合っている 2 本の脚を実際に寄せる。切ると溜めの演出だけになる")
    FBZZ_FIELD_RANGE(float, pullConvergence, 0.75f, "Convergence", 0.0f, 1.0f)
    FBZZ_TOOLTIP("満溜めで «2 本の中点» までどれだけ寄せるか。1 で完全に重なるので、"
                 "脚どうしがすれ違って見える手前で止める")
    FBZZ_FIELD_RANGE(float, pullIkWeight, 1.0f, "IK Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("IK の効き。溜め比の 2 乗に掛かるので、序盤はほとんど動かない")
    FBZZ_FIELD(bool, holdWhilePulling, true, "Hold Still")
    FBZZ_TOOLTIP("引かれている間ボスを歩かせない。接地した足を引きずるとスライドに見える")

    // 部位の当たり判定はレンダラーを持たないので、脚そのものは極が乗っても光らない。
    // 装甲を光らせるにはメッシュを部位単位で割る必要があるため、まずは «磁力クランプ» の
    // 輪を骨に巻いて «どの脚に何が乗っているか» を出す。
    FBZZ_GROUP("Leg Band")
    FBZZ_FIELD(bool, showBands, true, "Show Bands")
    FBZZ_FIELD_FILE(bandMaterial, "Assets/Materials/Surface/Unlit.mat", "Band Material", ".mat")
    FBZZ_TOOLTIP("輪の材質。Unlit なので albedo をそのまま明るさに使える")
    FBZZ_FIELD_RANGE(float, bandRadius, 0.62f, "Band Radius", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, bandThickness, 0.07f, "Band Thickness", 0.01f, 0.5f)
    FBZZ_FIELD_RANGE(float, bandBrightness, 3.0f, "Band Brightness", 0.0f, 12.0f)
    FBZZ_TOOLTIP("1 を超えるとブルームが拾う。輪は面積が小さいので白飛びしにくい")
    FBZZ_FIELD_RANGE(float, bandPullBoost, 2.5f, "Band Boost (pulling)", 1.0f, 8.0f)
    FBZZ_TOOLTIP("引き合っている間の上乗せ。溜まるほど明るくなる")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugCharged, 0, "Charged Parts")
    FBZZ_FIELD_READ_ONLY(std::string, debugPair, "-", "Pair")
    FBZZ_FIELD_READ_ONLY(float, debugPull, 0.0f, "Pull")

    /// 引き合いの進み [0,1]。HUD と発光が読む。
    [[nodiscard]] float PullRatio() const
    { return Clamp01(m_pull / std::max(pullSeconds, 0.01f)); }
    [[nodiscard]] bool IsPulling() const { return m_pull > 0.0f; }

    /// WHY 組み立てを開始時に済ませるか: コンポーネントの追加は ECS の格納そのものを
    ///     動かす。毎フレーム走る OnUpdate から行うと、他のシステムが巡回している
    ///     最中に配列が動きうる (PlayerHeadLookComponent と同じ理由)。
    void OnStart()  override { EnsureRuntime(); }
    void OnUpdate() override;

private:
    /// 脚 IK チェーンの order 起点。他が使っていない帯へ寄せて、脚 4 本ぶんを連番で持つ。
    static constexpr int kChainOrderBase = 700;

    /// 部位 1 つぶんの参照。位置は毎フレーム変わるので保持しない。
    struct Part {
        GameObject*                object = nullptr;
        BossPartPolarityComponent* part   = nullptr;
        Vector3                    position;
    };

    /// 自分の配下にある部位だけを集める。
    void CollectParts(std::vector<Part>& out) const;
    /// 異極の対を 1 組選ぶ。見つからなければ false。
    [[nodiscard]] bool PickPair(const std::vector<Part>& parts, int& a, int& b) const;
    /// 帯電している部位を極の色で囲う。毎フレーム。
    void DrawMarks(const std::vector<Part>& parts) const;
    /// 引き合っている間だけ、間隔の詰まる環と唸りを出す。
    void TickPullFeel(const Part& a, const Part& b, float dt);
    void DrawLink(const Part& a, const Part& b) const;
    void Fire(const Part& a, const Part& b);
    /// 表示用の短い部位名。"HB_Hock_FR" → "FR"。
    [[nodiscard]] static std::string ShortName(const GameObject& object);

    /// 脚の接尾辞 ("_FR") と BossLeg の番号を往復する。
    [[nodiscard]] static int         LegIndexOf(const std::string& suffix);
    [[nodiscard]] static const char* SuffixOf(int leg);

    /// GameObject を作る処理はここへ集める。
    ///
    /// WHY 1 箇所へ寄せるか: scene.Create は GameObject 配列を再確保する。部位を
    ///     走査しながら作ると、握っている GameObject* が途中で無効になる。
    ///     «作る» を先に済ませてから «読む» へ入る。
    void EnsureRuntime();
    void EnsureSolver();
    [[nodiscard]] IKChain* EnsureChain(int leg);
    /// 脚 1 本の «アニメーションが決めた足の位置»。
    [[nodiscard]] bool FootWorld(int leg, Vector3& out) const;

    void DrivePullIk(const Part& a, const Part& b);
    /// IK を全部畳む。対が崩れたら必ず通る。
    void ReleaseIk();
    void DriveBands(const std::vector<Part>& parts, bool pulling) const;

    /// 脚 1 本ぶんの IK 状態。
    struct LegIk {
        EntityRef target;
        EntityRef band;
        /// 引き始めの足の位置。ここから中点へ寄せる。
        Vector3   anchor;
        bool      held = false;
    };

    LegIk       m_legs[4];
    bool        m_runtimeBuilt = false;
    float       m_pull         = 0.0f;
    float       m_ringTimer    = 0.0f;
};

FBZZ_REFLECT(BossPolarityRigComponent)

inline std::string BossPolarityRigComponent::ShortName(const GameObject& object)
{
    const std::string& name = object.name;
    const std::size_t  cut  = name.find_last_of('_');
    return cut == std::string::npos ? name : name.substr(cut + 1);
}

inline int BossPolarityRigComponent::LegIndexOf(const std::string& suffix)
{
    if (suffix == "_FR") return 0;
    if (suffix == "_FL") return 1;
    if (suffix == "_BR") return 2;
    if (suffix == "_BL") return 3;
    return -1;
}

inline const char* BossPolarityRigComponent::SuffixOf(int leg)
{
    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    return kSuffix[std::clamp(leg, 0, 3)];
}

inline void BossPolarityRigComponent::EnsureSolver()
{
    GameObject* self = scene.Self();
    if (!self) return;

    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) ik = &self->AddComponent<IKSolverComponent>();
    ik->enabled = true;
}

inline void BossPolarityRigComponent::EnsureRuntime()
{
    GameObject* self = scene.Self();
    if (!self) return;

    // WHY 毎フレーム確かめ直すか: スクリプト DLL をリロードすると Script は作り直され、
    //     EntityRef は空へ戻る。一方で作った GameObject は Scene に残っているので、
    //     «作った» を覚えたままにすると輪と的が 1 組ずつ増え続ける。名前で拾い直す。
    if (m_runtimeBuilt && m_legs[0].target.Resolve(scene)) return;

    EnsureSolver();

    // WHY ループのたびに self を引き直すか: scene.Create は GameObject 配列を再確保する。
    //     ループの外で掴んだ self は 2 本目の生成で無効になり、FindInSubtree が
    //     解放済みの階層を辿る。
    for (int leg = 0; leg < 4; ++leg) {
        const std::string suffix = SuffixOf(leg);

        const std::string targetName = "BossLegIkTarget" + suffix;
        GameObject* target = scene.Find(targetName);
        if (!target) {
            GameObject& created  = scene.Create(targetName);
            created.runtimeGenerated = true;
            target = &created;
        }
        m_legs[leg].target = EntityRef{ target->GetID() };

        // 輪は当たり判定の子に付ける。あの GameObject はローカル +Y が骨の向きへ
        // 揃えてあるので (BossHitboxRigComponent::AlignUpTo)、そのまま脚に巻ける。
        GameObject* owner = scene.Self();
        if (!owner || !FindInSubtree(*owner, std::string("HB_Hock") + suffix)) continue;

        const std::string bandName = "BossLegBand" + suffix;
        GameObject* band = scene.Find(bandName);
        if (!band) {
            GameObject& created = scene.Create(bandName);
            created.runtimeGenerated = true;
            band = &created;
        }
        m_legs[leg].band = EntityRef{ band->GetID() };
    }

    // 親付けと形の組み立ては «全部作り終えてから» 行う。生成ループの中で掴んだ
    // ポインタは次の Create で無効になる。
    for (int leg = 0; leg < 4; ++leg) {
        GameObject* band = m_legs[leg].band.Resolve(scene);
        GameObject* owner = scene.Self();
        if (!band || !owner) continue;

        if (GameObject* hitbox =
                FindInSubtree(*owner, std::string("HB_Hock") + SuffixOf(leg))) {
            band->SetParent(*hitbox);
            band->transform.position = Vector3::ZERO;
            band->transform.rotation = Quaternion::Identity();
        }

        auto* procedural = band->GetComponent<ProceduralMeshComponent>();
        if (!procedural) procedural = &band->AddComponent<ProceduralMeshComponent>();
        if (!bandMaterial.empty()) procedural->materialPath = bandMaterial;

        MeshBuilder builder;
        builder.AddTorus(Vector3::ZERO, Vector3::UP,
                         std::max(bandRadius, 0.05f), std::max(bandThickness, 0.01f), 24, 8);
        mesh.Apply(*band, builder);
    }

    m_runtimeBuilt = true;
}

inline IKChain* BossPolarityRigComponent::EnsureChain(int leg)
{
    GameObject* self   = scene.Self();
    GameObject* target = m_legs[leg].target.Resolve(scene);
    if (!self || !target) return nullptr;

    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) return nullptr;

    const int order = kChainOrderBase + leg;
    for (IKChain& chain : ik->chains) {
        if (chain.type == IKSolverType::FABRIK && chain.order == order) {
            // 的はランタイム生成なのでシーンに残らない。参照は毎回張り直す。
            chain.targetEntity = target->GetID();
            return &chain;
        }
    }

    const std::string suffix = SuffixOf(leg);
    IKChain chain{};
    chain.type  = IKSolverType::FABRIK;
    // README のリグ構成どおり 4 節。TwoBone は 3 本しか受けないので FABRIK を使う。
    chain.boneNames = { "Thigh" + suffix, "Shin" + suffix, "Hock" + suffix, "Foot" + suffix };
    chain.order        = order;
    chain.enabled      = false;
    chain.weight       = 0.0f;
    chain.targetEntity = target->GetID();
    ik->chains.push_back(std::move(chain));
    return &ik->chains.back();
}

inline bool BossPolarityRigComponent::FootWorld(int leg, Vector3& out) const
{
    const auto* rig = scene.GetScript<BossHitboxRigComponent>();
    if (!rig) return false;
    GameObject* foot = rig->FootBone(static_cast<BossLeg>(leg));
    if (!foot) return false;
    out = foot->transform.worldPosition;
    return true;
}

inline void BossPolarityRigComponent::DrivePullIk(const Part& a, const Part& b)
{
    if (!pullLegs) return;

    const int ia = LegIndexOf(a.part->legSuffix);
    const int ib = LegIndexOf(b.part->legSuffix);
    if (ia < 0 || ib < 0 || ia == ib) return;

    // 引き始めの足の位置を 1 度だけ控える。
    //
    // WHY 毎フレーム読み直さないか: IK が効き始めると足のワールド位置は «IK が動かした
    //     後» の値になる。それを次のフレームの基準にすると、自分の出力を入力に混ぜて
    //     脚がじりじり寄り続ける (溜めが 0 でも戻らなくなる)。
    const int legs[2] = { ia, ib };
    for (const int leg : legs) {
        if (m_legs[leg].held) continue;
        if (!FootWorld(leg, m_legs[leg].anchor)) return;
        m_legs[leg].held = true;
    }

    const Vector3 midpoint = (m_legs[ia].anchor + m_legs[ib].anchor) * 0.5f;
    // 序盤をほとんど動かさないために 2 乗で効かせる。線形だと «塗った瞬間から
    // ずっと脚が寄っている» に見えて、溜めの終わりが立たない。
    const float ratio = PullRatio();
    const float ease  = ratio * ratio;

    for (const int leg : legs) {
        IKChain* chain = EnsureChain(leg);
        if (!chain) continue;

        GameObject* target = m_legs[leg].target.Resolve(scene);
        if (!target) continue;

        const Vector3 goal =
            Vector3::Lerp(m_legs[leg].anchor, midpoint, ease * Clamp01(pullConvergence));
        // 的はルート直下なのでローカルとワールドが一致するが、両方書く
        // (TransformSystem が回る前に IKSystem が読む経路があるため)。
        target->transform.position      = goal;
        target->transform.worldPosition = goal;

        chain->enabled = true;
        chain->weight  = Clamp01(ease * Max(pullIkWeight, 0.0f));
    }

    if (holdWhilePulling)
        if (auto* ai = scene.GetScript<BossAiComponent>()) ai->SetRestrained(true);
}

inline void BossPolarityRigComponent::ReleaseIk()
{
    GameObject* self = scene.Self();
    if (!self) return;

    for (LegIk& leg : m_legs) leg.held = false;

    if (auto* ik = self->GetComponent<IKSolverComponent>())
        for (IKChain& chain : ik->chains)
            if (chain.type == IKSolverType::FABRIK && chain.order >= kChainOrderBase) {
                chain.enabled = false;
                chain.weight  = 0.0f;
            }

    if (auto* ai = scene.GetScript<BossAiComponent>()) ai->SetRestrained(false);
}

inline void BossPolarityRigComponent::DriveBands(const std::vector<Part>& parts,
                                                 bool pulling) const
{
    static constexpr MaterialPropertyId kAlbedoId{ "albedo" };

    for (int leg = 0; leg < 4; ++leg) {
        GameObject* band = m_legs[leg].band.Resolve(scene);
        if (!band) continue;

        // その脚の部位を引く。見つからない構成 (Hock が無い) では輪ごと伏せる。
        const BossPartPolarityComponent* part = nullptr;
        for (const Part& candidate : parts)
            if (LegIndexOf(candidate.part->legSuffix) == leg) { part = candidate.part; break; }

        const bool lit = showBands && part && part->IsCharged();
        band->SetActive(lit);
        if (!lit) continue;

        // 切れる直前は暗くする。«まだ乗っている» と «もう切れる» が同じ明るさだと、
        // 2 本目を入れに行くか諦めるかの判断ができない。
        float gain = Max(bandBrightness, 0.0f) *
                     FadeFromRemaining(part->RemainingNormalized());
        if (pulling) gain *= Lerp(1.0f, Max(bandPullBoost, 1.0f), PullRatio());

        const Vector4 base = PolarityColor(part->Current());
        material.Instance(EntityRef{ band->GetID() })
            .SetVector4(kAlbedoId, { base.x * gain, base.y * gain, base.z * gain, 1.0f });
    }
}

inline void BossPolarityRigComponent::CollectParts(std::vector<Part>& out) const
{
    out.clear();
    GameObject* self = scene.Self();
    if (!self) return;

    for (GameObject* object : scene.FindObjectsOfType<BossPartPolarityComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        // 盤面に複数のボスが居ても、対を組むのは自分の部位どうしだけ。
        if (BossHitboxRigComponent::BossRootOf(object) != self) continue;

        auto* part = scene.GetScript<BossPartPolarityComponent>(object);
        if (!part) continue;

        out.push_back(Part{ object, part, object->transform.worldPosition });
    }
}

inline bool BossPolarityRigComponent::PickPair(const std::vector<Part>& parts,
                                               int& a, int& b) const
{
    const float radiusSq = std::max(pairRadius, 0.0f) * std::max(pairRadius, 0.0f);
    // WHY «残りが少ない方» を優先して選ぶか: 先に塗った側から切れていくので、
    //     組めるうちに組む対を選ばないと、対が成立した直後に片方が切れて
    //     溜めが毎回ふりだしへ戻る。
    float best = -1.0f;
    bool  found = false;

    const int count = static_cast<int>(parts.size());
    for (int i = 0; i < count; ++i) {
        if (!parts[i].part->IsCharged()) continue;
        for (int j = i + 1; j < count; ++j) {
            if (!parts[j].part->IsCharged()) continue;
            if (!IsAttracting(parts[i].part->Current(), parts[j].part->Current())) continue;
            if ((parts[j].position - parts[i].position).LengthSq() > radiusSq) continue;

            const float urgency = -std::min(parts[i].part->Remaining(),
                                            parts[j].part->Remaining());
            if (!found || urgency > best) {
                best  = urgency;
                a     = i;
                b     = j;
                found = true;
            }
        }
    }
    return found;
}

inline void BossPolarityRigComponent::DrawLink(const Part& a, const Part& b) const
{
    if (!drawPullLink) return;

    // 2 部位の色を混ぜたうえで、溜まるほど白へ寄せる。どちらの極かは両端の環が
    // 言うので、線は «どれだけ張り詰めたか» だけを担当する。
    const float   ratio = PullRatio();
    const Vector4 ca    = PolarityColor(a.part->Current());
    const Vector4 cb    = PolarityColor(b.part->Current());
    const Vector4 color{ Lerp((ca.x + cb.x) * 0.5f, 1.0f, ratio),
                         Lerp((ca.y + cb.y) * 0.5f, 1.0f, ratio),
                         Lerp((ca.z + cb.z) * 0.5f, 1.0f, ratio), 1.0f };
    debug.DrawLine(a.position, b.position, color);
}

inline void BossPolarityRigComponent::DrawMarks(const std::vector<Part>& parts) const
{
    if (!drawPartMarks) return;

    for (const Part& part : parts) {
        if (!part.part->IsCharged()) continue;
        // 切れる直前は薄くする。«まだ乗っている» と «もう切れる» を同じ濃さで出すと、
        // 2 本目を入れに行くか諦めるかの判断ができない。
        Vector4 color = PolarityColor(part.part->Current());
        color.w = FadeFromRemaining(part.part->RemainingNormalized());
        debug.DrawSphere(part.position, std::max(markRadius, 0.05f), color);
    }
}

inline void BossPolarityRigComponent::TickPullFeel(const Part& a, const Part& b, float dt)
{
    m_ringTimer -= dt;
    if (m_ringTimer > 0.0f) return;

    // 間隔が詰まっていくことで «来る» を出す。同じ間隔で刻むと、溜まっているのか
    // ただ乗っているだけなのかが音からも絵からも読めない。
    const float ratio = PullRatio();
    m_ringTimer = Lerp(std::max(ringIntervalFar, 0.02f),
                       std::max(ringIntervalNear, 0.02f), ratio);

    if (auto* rings = PolarityRingComponent::Instance()) {
        rings->Burst(a.position, ringRadius, a.part->Current());
        rings->Burst(b.position, ringRadius, b.part->Current());
    }
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.10f + 0.35f * ratio, 0.05f + 0.20f * ratio, 0.06f);
    se::Play(audio, se::kAttractWindup, 0.35f + 0.65f * ratio);
}

inline void BossPolarityRigComponent::Fire(const Part& a, const Part& b)
{
    const Vector3 midpoint = (a.position + b.position) * 0.5f;
    const Polarity shown   = a.part->Current();

    // 使った極は落とす。残したままだと転倒から復帰した瞬間に同じ対が再成立して、
    // プレイヤーが何もしていないのに 2 度目が始まる。
    a.part->Clear();
    b.part->Clear();
    m_pull      = 0.0f;
    m_ringTimer = 0.0f;
    // 転倒モーションが脚を持っていくので、IK は必ずここで手を離す。残すと
    // 倒れている最中も足が引き寄せ先へ引っ張られ、崩れ方が毎回違って見える。
    ReleaseIk();

    if (auto* ai = scene.GetScript<BossAiComponent>())
        ai->Topple(toppleSeconds, std::max(toppleSelfDamage, 0));

    if (auto* stop = HitstopManagerComponent::Instance())  stop->Hit(Clamp01(toppleHitStop));
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(Clamp01(toppleShake));
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.95f, 0.7f, 0.35f);
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Flash(PolarityColor(shown), Clamp01(toppleFlash), 0.18f);
    if (auto* rings = PolarityRingComponent::Instance())
        rings->Burst(midpoint, ringRadius * 3.0f, shown);

    se::Play(audio, se::kAttractConverge);
    se::Play(audio, se::kImpactHeavy);
}

inline void BossPolarityRigComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    // 生成は必ず走査より前。scene.Create が GameObject 配列を伸ばすので、
    // parts が握っているポインタを跨いで作ると途中で無効になる。
    EnsureRuntime();

    std::vector<Part> parts;
    CollectParts(parts);

    debugCharged = 0;
    for (const Part& part : parts)
        if (part.part->IsCharged()) ++debugCharged;

    // 既に倒れている間は次の対を溜めない。倒れている 5 秒のあいだに組み上がると、
    // 起き上がった瞬間にもう一度転ぶ ─ プレイヤーから見て «起きない敵» になる。
    const auto* core       = scene.GetScript<BossPolarityCoreComponent>();
    const bool  isDown     = core && core->IsStaggered();

    int a = 0;
    int b = 0;
    const bool paired = !isDown && PickPair(parts, a, b);

    DrawMarks(parts);

    if (paired) {
        m_pull += dt;
        DrawLink(parts[a], parts[b]);
        TickPullFeel(parts[a], parts[b], dt);
        DrivePullIk(parts[a], parts[b]);
        debugPair = ShortName(*parts[a].object) + PolaritySymbol(parts[a].part->Current()) +
                    " / " +
                    ShortName(*parts[b].object) + PolaritySymbol(parts[b].part->Current());
    } else {
        m_pull      = std::max(0.0f, m_pull - dt * std::max(pullDecay, 0.0f));
        m_ringTimer = 0.0f;
        debugPair   = "-";
        ReleaseIk();
    }

    DriveBands(parts, paired);
    debugPull = PullRatio();

    if (paired && m_pull >= std::max(pullSeconds, 0.01f))
        Fire(parts[a], parts[b]);
}

} // namespace sandbox
