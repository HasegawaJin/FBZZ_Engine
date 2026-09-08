/// @file    PlayerClimbComponent.hpp
/// @brief   倒れたボスの脚を登って甲板へ上がる。刀は登るあいだ背中へ納める
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// WHY 転倒中しか登れないようにするか:
///   歩いているボスへ取り付くと «動く床の上でプレイヤーを追従させる» ことになり、
///   毎フレーム骨の世界姿勢を引いて座標を合わせ続ける必要が出る。転倒中に限れば
///   ボスはほぼ静止していて、この問題が丸ごと消える (Docs/climb-core.md)。
///
/// WHY 経路を骨から引くか (座標を直書きせず):
///   脚は 3 リンクで、転倒すると畳まれる。足・踵関節・膝の «今の» 世界位置を
///   毎回引けば、どんな姿勢で倒れていても経路がその形に沿う。
///   直書きした座標は «立っているボス» にしか合わない。
///
/// WHY 納刀を挟むか:
///   両手が塞がったまま登る絵は «刀が壁を貫く» か «手が離れている» のどちらかにしかならない。
///   抜刀・納刀のイベント (SOCKET_BackSword_*) は既にあるので、挟むだけで済む。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerClimbComponent : public Script {
    FBZZ_SCRIPT(PlayerClimbComponent)

public:
    FBZZ_GROUP("取り付き")
    FBZZ_FIELD_RANGE(float, mountRange, 3.6f, "取り付ける距離", 0.5f, 12.0f)
    FBZZ_TOOLTIP("脚の «足» からこの距離まで寄ると登れる。とどめ の 3.4m と揃えてある")
    FBZZ_FIELD(std::string, mountAction, actions::kClimb, "入力")
    FBZZ_TOOLTIP("取り付く入力。既定は専用の «登る» (F / パッド Y)。ジャンプと兼ねると «跳ぼうとして登る» が起きる")
    FBZZ_TOOLTIP("取り付きの入力。とどめ (Parry) と competing しないよう跳躍を使う")

    FBZZ_GROUP("速さ")
    FBZZ_FIELD_RANGE(float, climbSpeed, 3.4f, "登る速さ", 0.5f, 20.0f)
    FBZZ_TOOLTIP("経路上を進む速さ [m/s]。総距離は約 5m なので 1.5 秒前後になる")
    FBZZ_FIELD_RANGE(float, sheatheSeconds, 0.62f, "納刀", 0.0f, 3.0f)
    FBZZ_TOOLTIP("Katana_Sheathe は 40F/1.33s だが、刀が背中へ移るのは f22 (0.73s)。"
                 "そこまで待てば絵は繋がる")
    FBZZ_FIELD_RANGE(float, drawSeconds, 0.60f, "抜刀", 0.0f, 3.0f)
    FBZZ_TOOLTIP("Katana_Draw は 34F/1.13s、刀が手へ移るのは f18 (0.60s)")

    FBZZ_GROUP("甲板")
    FBZZ_FIELD(std::string, deckAnchorBone, "Body", "基準の骨")
    FBZZ_TOOLTIP("甲板の高さを測る基準。ボスの胴の骨から真上へ deckRise 上げた点に立たせる")
    FBZZ_FIELD_RANGE(float, deckRise, 1.33f, "甲板の高さ", 0.0f, 6.0f)
    FBZZ_TOOLTIP("胴の骨から甲板までの高さ [m]。Blender 実測で 5.83 − 4.50 = 1.33")
    FBZZ_FIELD_RANGE(float, deckSideOffset, 1.55f, "蓋からの距離", 0.0f, 4.0f)
    FBZZ_FIELD_RANGE(float, holdSeconds, 3.2f, "乗っている間の猶予 [s]", 0.0f, 10.0f)
    FBZZ_TOOLTIP("背に乗っているあいだ «あと何秒» 倒れたままにするか。降りるとここから数えて起き上がる。ボス側の上限 (9 秒) を超えては延ばせない")
    FBZZ_TOOLTIP("着地点を蓋の中心からどれだけ横へずらすか [m]。0 だとコアに埋まる")

    FBZZ_GROUP("クリップ")
    FBZZ_FIELD(std::string, layerName, "Attack", "Layer")
    FBZZ_FIELD_FILE(climbClipFile,
        "guid:2c331a3fc4289104bf623dda30f469ad|Library/Baked/2c331a3fc4289104bf623dda30f469ad/anims/Climb.anim",
        "Climb Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, climbClipName, "Climb", "Climb Clip Name")
    FBZZ_FIELD_FILE(sheatheClipFile,
        "guid:bf25e66cf0b7067fb10270753895d369|Library/Baked/bf25e66cf0b7067fb10270753895d369/anims/Katana_Sheathe.anim",
        "Sheathe Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, sheatheClipName, "Katana_Sheathe", "Sheathe Clip Name")
    FBZZ_FIELD_FILE(drawClipFile,
        "guid:9873c746d0c7f993161455bdcad44fd2|Library/Baked/9873c746d0c7f993161455bdcad44fd2/anims/Katana_Draw.anim",
        "Draw Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, drawClipName, "Katana_Draw", "Draw Clip Name")

    FBZZ_GROUP("デバッグ")
    // WHY 転倒条件を外せるようにするか: 登りの «見た目» を直すたびにボスを転倒させるのは
    //     現実的でない。これは確認用の足場であって、遊びの条件ではない。
    //     既定は true のままなので、外さなければ挙動は変わらない。
    FBZZ_GROUP("音")
    FBZZ_FIELD_RANGE(float, climbVolume, 0.95f, "登攀の音量", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, grabInterval, 0.4165f, "手を掛ける間隔 [s]", 0.05f, 2.0f)
    FBZZ_TOOLTIP("Climb クリップ (25F/0.83s) の半分。左右 1 回ずつ手が掛かるので、クリップの尺を変えたらここも半分に合わせる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD(bool, requireToppled, true, "転倒中だけ登れる")
    FBZZ_TOOLTIP("外すと «いつでも脚に取り付ける»。モーション確認用で、遊びの条件ではない")
    FBZZ_FIELD(bool, drawPath, true, "経路を描く")
    FBZZ_FIELD_READ_ONLY(std::string, debugPhase, "None", "状態")
    FBZZ_FIELD_READ_ONLY(float, debugProgress, 0.0f, "進捗 [m]")
    FBZZ_FIELD_READ_ONLY(std::string, debugLeg, "", "登っている脚")

    /// 登っている / 甲板に居る。移動と攻撃を止める側が読む。
    [[nodiscard]] bool IsClimbing() const { return m_phase != Phase::None && m_phase != Phase::Deck; }
    [[nodiscard]] bool IsOnDeck()   const { return m_phase == Phase::Deck; }
    /// 甲板から降ろす。ボスが起き上がったとき・撃破されたときに呼ぶ。
    void Dismount();

    void OnStart() override;
    void OnUpdate() override;
    void OnDrawGizmos() override;

private:
    /// 今このボスに取り付けるか。既定では転倒中だけ。
    [[nodiscard]] bool Climbable(const IBoss* boss) const;
    enum class Phase { None, Sheathe, Climb, Draw, Deck };

    /// 一番近い脚の «足» を探す。見つからなければ false。
    [[nodiscard]] bool FindNearestLeg(std::string& outSuffix, Vector3& outFoot) const;
    /// 足 → 踵関節 → 膝 → 甲板 の 4 点を今の姿勢から引き直す。
    void BuildPath(const std::string& suffix);
    /// 経路上を distance だけ進んだ点。
    [[nodiscard]] Vector3 PointAt(float distance) const;
    [[nodiscard]] float   PathLength() const;

    [[nodiscard]] GameObject* Boss() const;
    [[nodiscard]] static GameObject* FindInSubtree(GameObject& root, const std::string& name);

    Phase                m_phase   = Phase::None;
    float                m_timer   = 0.0f;
    float                m_travel  = 0.0f;   // 経路上をどこまで進んだか [m]
    /// 手掛けの位相 [秒]。クリップの周期に合わせて鳴らすため。
    float m_grabTimer  = 0.0f;
    /// 二刀の 2 本目までの残り [秒]。0 以下で «鳴らさない»。
    float m_stowSecond = 0.0f;
    float m_drawSecond = 0.0f;
    std::string          m_suffix;
    std::vector<Vector3> m_path;
    EntityRef            m_boss;
};

FBZZ_REFLECT(PlayerClimbComponent)

inline GameObject* PlayerClimbComponent::Boss() const
{
    if (GameObject* cached = m_boss.Resolve(scene)) return cached;
    return FindBossOnBoard(scene);
}

inline GameObject* PlayerClimbComponent::FindInSubtree(GameObject& root, const std::string& name)
{
    if (root.name == name) return &root;
    const int count = root.GetChildCount();
    for (int i = 0; i < count; ++i)
        if (GameObject* child = root.GetChild(i))
            if (GameObject* found = FindInSubtree(*child, name)) return found;
    return nullptr;
}

inline bool PlayerClimbComponent::FindNearestLeg(std::string& outSuffix, Vector3& outFoot) const
{
    GameObject* boss = Boss();
    if (!boss) return false;

    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    const Vector3 origin = transform.worldPosition;

    bool  found = false;
    float best  = 0.0f;
    for (const char* suffix : kSuffix) {
        GameObject* foot = FindInSubtree(*boss, std::string("Foot") + suffix);
        if (!foot) continue;
        Vector3 delta = foot->transform.worldPosition - origin;
        delta.y = 0.0f;
        const float distance = delta.Length();
        if (!found || distance < best) {
            found = true; best = distance;
            outSuffix = suffix; outFoot = foot->transform.worldPosition;
        }
    }
    return found && best <= std::max(mountRange, 0.1f);
}

inline void PlayerClimbComponent::BuildPath(const std::string& suffix)
{
    m_path.clear();
    GameObject* boss = Boss();
    if (!boss) return;

    // 脚は 3 リンク。足 → 踵関節 → 膝 の «今の» 世界位置をそのまま踊り場にする。
    for (const char* bone : { "Foot", "Hock", "Thigh" }) {
        if (GameObject* node = FindInSubtree(*boss, std::string(bone) + suffix))
            m_path.push_back(node->transform.worldPosition);
    }

    // 甲板の着地点。胴の骨から真上へ上げ、蓋の中心から横へずらす。
    if (GameObject* body = FindInSubtree(*boss, deckAnchorBone)) {
        const Vector3 up   = boss->transform.up;
        Vector3       side = boss->transform.right;
        // 登ってきた脚の側へ降ろす。反対側へ出すと甲板を横断してから戦うことになる。
        if (!m_path.empty()) {
            Vector3 lean = m_path.front() - body->transform.worldPosition;
            lean.y = 0.0f;
            if (lean.LengthSq() > EPSILON) side = lean.Normalized();
        }
        const Vector3 deck = body->transform.worldPosition
                           + up * std::max(deckRise, 0.0f)
                           + side * std::max(deckSideOffset, 0.0f);
        m_path.push_back(deck);
    }
}

inline float PlayerClimbComponent::PathLength() const
{
    float total = 0.0f;
    for (std::size_t i = 1; i < m_path.size(); ++i)
        total += (m_path[i] - m_path[i - 1]).Length();
    return total;
}

inline Vector3 PlayerClimbComponent::PointAt(float distance) const
{
    if (m_path.empty()) return transform.worldPosition;
    float walked = 0.0f;
    for (std::size_t i = 1; i < m_path.size(); ++i) {
        const Vector3 segment = m_path[i] - m_path[i - 1];
        const float   length  = segment.Length();
        if (length <= EPSILON) continue;
        if (walked + length >= distance)
            return m_path[i - 1] + segment * ((distance - walked) / length);
        walked += length;
    }
    return m_path.back();
}

inline void PlayerClimbComponent::Dismount()
{
    if (m_phase == Phase::None) return;
    m_phase = Phase::None;
    m_timer = 0.0f;
    m_travel = 0.0f;
    m_path.clear();
    animator.StopSlot(layerName);
    cutscene::Publish(/*holdBoss=*/false, /*holdPlayer=*/false, Time::time);
}

inline void PlayerClimbComponent::OnStart()
{
    m_phase = Phase::None; m_timer = 0.0f; m_travel = 0.0f;
    m_grabTimer = 0.0f; m_stowSecond = 0.0f; m_drawSecond = 0.0f;
    m_path.clear(); m_boss = {}; m_suffix.clear();
    debugPhase = "None"; debugProgress = 0.0f; debugLeg.clear();
}

inline bool PlayerClimbComponent::Climbable(const IBoss* boss) const
{
    return boss && (!requireToppled || boss->IsToppled());
}

inline void PlayerClimbComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    // 二刀は «必ず 2 回鳴る» のが素材側の前提。1 本目は状態が変わった所で鳴らし、
    // 2 本目はここで少し遅らせて出す ─ 同時に鳴らすと 1 本の太い音になる。
    if (m_stowSecond > 0.0f && (m_stowSecond -= dt) <= 0.0f)
        se::Play(audio, se::kPlayerWeaponStow, climbVolume * 0.92f);
    if (m_drawSecond > 0.0f && (m_drawSecond -= dt) <= 0.0f)
        se::Play(audio, se::kPlayerWeaponDeploy, climbVolume * 0.92f);

    GameObject* boss = Boss();
    const IBoss* iboss = IBoss::Of(boss);

    // 乗っているあいだは倒れたままにしてもらう。
    // WHY 毎フレーム «あと holdSeconds 秒» と言い続けるか: 降りた瞬間に言うのをやめれば、
    //     ボスは holdSeconds 後に起き上がる。«いつ降りたか» を別に伝える必要がない。
    if (m_phase != Phase::None && boss)
        if (auto* ai = scene.GetScript<BossAiComponent>(boss))
            ai->HoldTopple(std::max(holdSeconds, 0.0f));

    // 登っている最中にボスが起き上がった / 倒された → 降ろす。
    if (m_phase != Phase::None && (!iboss || (!Climbable(iboss) && m_phase != Phase::Deck)))
        Dismount();

    switch (m_phase) {
    case Phase::None: {
        if (!Climbable(iboss)) break;
        if (!input.GetActionDown(mountAction)) break;
        std::string suffix; Vector3 foot;
        if (!FindNearestLeg(suffix, foot)) break;

        m_suffix = suffix;
        m_boss   = boss ? EntityRef{ boss->GetID() } : EntityRef{};
        BuildPath(suffix);
        if (m_path.size() < 2) break;

        m_phase = Phase::Sheathe; m_timer = 0.0f; m_travel = 0.0f;
        // 登っているあいだプレイヤーの入力を止める。ボスは転倒の演出が持っている。
        cutscene::Publish(/*holdBoss=*/false, /*holdPlayer=*/true, Time::time);
        // 刀を背へ回す音。二刀なので «必ず 2 回鳴る» のが素材側の前提
        // (Assets/Sound/SE/README_v3_Blades_Arena.md)。ここは 1 本目。
        se::Play(audio, se::kPlayerWeaponStow, climbVolume);
        m_stowSecond = 0.11f;
        if (!sheatheClipFile.empty())
            animator.PlaySlot(layerName, sheatheClipFile, sheatheClipName, 0.08f, 0.10f, 1.0f, false);
        debugLeg = suffix;
        break;
    }
    case Phase::Sheathe:
        m_timer += dt;
        if (m_timer >= std::max(sheatheSeconds, 0.0f)) {
            m_phase = Phase::Climb; m_timer = 0.0f;
            if (!climbClipFile.empty())
                animator.PlaySlot(layerName, climbClipFile, climbClipName, 0.10f, 0.12f, 1.0f, true);
        }
        break;
    case Phase::Climb: {
        // 経路は毎フレーム引き直す。転倒の揺り戻しで脚が動いても付いていける。
        BuildPath(m_suffix);
        m_travel += std::max(climbSpeed, 0.1f) * dt;
        // WHY 距離ではなく時間で鳴らすか: 手が掛かる回数を決めているのは Climb クリップ
        //     (25F/0.83s に左右 1 回ずつ) であって climbSpeed ではない。距離で鳴らすと
        //     速さを変えた瞬間に «手は動いていないのに音だけ鳴る» になる。
        m_grabTimer += dt;
        const float stride = std::max(grabInterval, 0.05f);
        while (m_grabTimer >= stride) {
            m_grabTimer -= stride;
            se::Play(audio, se::kPlayerClimbGrab, climbVolume);
        }
        const float total = PathLength();
        transform.worldPosition = PointAt(std::min(m_travel, total));
        debugProgress = m_travel;
        if (m_travel >= total) {
            m_phase = Phase::Draw; m_timer = 0.0f;
            animator.StopSlot(layerName, 0.08f);
            if (!drawClipFile.empty())
                animator.PlaySlot(layerName, drawClipFile, drawClipName, 0.08f, 0.10f, 1.0f, false);
            se::Play(audio, se::kPlayerWeaponDeploy, climbVolume);
            m_drawSecond = 0.13f;
        }
        break;
    }
    case Phase::Draw:
        m_timer += dt;
        if (m_timer >= std::max(drawSeconds, 0.0f)) {
            m_phase = Phase::Deck; m_timer = 0.0f;
            // 甲板に着いたら操作を返す。ここから先は普通の戦闘。
            cutscene::Publish(false, false, Time::time);
            // 着地音を «甲板に乗った» に流用する。専用の音を足さないのは、
            // 高さから落ちた着地と «乗り移った» が体の側では同じ出来事だから。
            se::Play(audio, se::kPlayerLanding, climbVolume * 0.85f);
        }
        break;
    case Phase::Deck:
        // ボスが起き上がる / 撃破される → 降りる。
        // WHY 位置を毎フレーム貼り直さないか: 甲板に立ったら普通の移動へ戻すので、
        //     ここで座標を握り続けると甲板の上を歩けなくなる。ボスが動き出したら
        //     どのみち降ろすので、追従は要らない。
        if (!Climbable(iboss)) Dismount();
        break;
    }

    static constexpr const char* kName[] = { "None", "Sheathe", "Climb", "Draw", "Deck" };
    debugPhase = kName[static_cast<int>(m_phase)];
}

inline void PlayerClimbComponent::OnDrawGizmos()
{
    if (!drawPath || m_path.size() < 2) return;
    for (std::size_t i = 1; i < m_path.size(); ++i)
        debug.DrawLine(m_path[i - 1], m_path[i], { 0.2f, 1.0f, 0.45f, 1.0f });
}

} // namespace sandbox
