/// @file    Boss03HitboxRigComponent.hpp
/// @brief   Boss03 の翼 6 枚の当たり判定を、骨から組み立てる
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// @note 翼 6 枚は同形で角度だけが違う。手置きだと寸法の食い違いが混ざり、リグ書き出しのたびに置き直しになるため骨のローカル軸 (+Y) から組み立てる。
/// @note `Boss03AiComponent::Execute` は当たりの名前から翼を引く (`Boss03WingFromName`)。骨名を含む名前 (`HB_Wing_L_Upper` 等) にして対応表を 1 か所に閉じる。
/// @note 翼を落とすのはとどめのみ (`Docs/break-parry.md`)。耐久は「斬った手応え」用の器で、削り切ると斬撃対象から外れるため高めに設定する。
/// @note 斬撃は物理でなく `BossPartComponent` を型で集めて判定する (`BladeComponent`)。非トリガーが要るのはプレイヤーの衝突用で、このボスは頭上に浮き衝突しない。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/Boss03AnimParams.hpp>
#include <Scripts/Combat/Boss03AnimatorComponent.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// @note `Scene.hpp` まで要るのは `AddComponent`/`AddScript` の template 本体が末尾にあるため (`GameObject.hpp` は前方宣言のみ)。
class Boss03HitboxRigComponent : public Script {
    FBZZ_SCRIPT(Boss03HitboxRigComponent)

public:
    FBZZ_GROUP("寸法")
    FBZZ_FIELD_RANGE(float, bodyRadius, 1.40f, "胴", 0.05f, 6.0f)
    FBZZ_TOOLTIP("胴の斬撃判定。ダメージは本体HPへ返す。とどめで切断する対象にはしない")
    FBZZ_FIELD_RANGE(float, wingRadius, 0.55f, "翼の太さ", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, wingLength, 2.40f, "翼の長さ", 0.2f, 10.0f)
    FBZZ_TOOLTIP("骨の根元から翼先までの長さ [m]。骨のローカル +Y 方向へ伸ばす。"
                 "**モデルに «先端» の骨が無いので寸法はここが正本** ─ "
                 "見ながら合わせること")
    FBZZ_FIELD_RANGE(float, radiusScale, 1.0f, "全体スケール", 0.1f, 3.0f)
    FBZZ_TOOLTIP("太さだけを一括で動かす。«当たりの甘さ» の調整用")

    FBZZ_GROUP("耐久")
    FBZZ_FIELD_RANGE_INT(int, wingHealth, 480, "翼", 1, 5000)
    FBZZ_TOOLTIP("翼 1 枚が斬撃を受け止められる量。**削り切っても落ちない** ─ "
                 "落とすのは とどめ だけ。0 になると斬撃の対象から外れるので高めに置く")

    FBZZ_GROUP("レイヤー")
    FBZZ_FIELD_RANGE_INT(int, hitboxLayer, 9, "当たりのレイヤー", 0, 31)
    FBZZ_TOOLTIP("生成する当たりに入れるレイヤー番号。ボス本体と分けておく")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugHitboxes, 0, "ヒットボックス")
    FBZZ_FIELD_READ_ONLY(int, debugMissingBones, 0, "見つからないボーン")

    void OnStart()  override;
    void OnUpdate() override;

    /// 翼 i の当たり。まだ組めていなければ nullptr。
    [[nodiscard]] GameObject* WingHitbox(int wing) const
    {
        if (wing < 0 || wing >= kBoss03WingCount) return nullptr;
        return m_wings[wing].Resolve(scene);
    }
    /// 翼 i の骨。VFX が光の取り付け先として使う。
    [[nodiscard]] GameObject* WingBone(int wing) const;

private:
    /// 当たり 1 つぶんの宣言。
    struct Piece {
        std::string bone;
        float       radius = 0.5f;
        /// 骨の +Y へ伸ばす長さ [m]。0 なら球。
        float       length = 0.0f;
        /// 斬れる部位か (BossPartComponent を付ける)。
        int         wing   = -1;
    };

    void Build();
    /// 骨が居れば当たりを 1 つ作る。まだ骨が無ければ false。
    bool BuildPiece(const Piece& piece);
    /// 落ちた翼の当たりを畳む。
    void SyncDetached();
    /// 斬られた翼を見つけて、加算レイヤーへ仰け反りを 1 発差し込む。
    void SyncFlinch();

    [[nodiscard]] Boss03AnimatorComponent* Anim() const
    { return scene.GetScript<Boss03AnimatorComponent>(); }

    /// @note ボスの骨 GameObject はシーンに保存されず AnimatorSystem が実行時に作るため、OnStart 時点では 1 本も無いことがある。揃うまで間を置いて試し続ける。
    std::vector<Piece> m_pending;
    float     m_waited        = 0.0f;
    float     m_retryCooldown = 0.0f;
    bool      m_reported      = false;
    EntityRef m_wings[kBoss03WingCount];
    /// 前フレームの «斬られた閃光»。立ち上がりだけを 1 発として拾う。
    float     m_flash[kBoss03WingCount] = {};

    static constexpr float kRetryInterval = 0.20f;
    static constexpr float kGiveUpSeconds = 6.0f;
};

FBZZ_REFLECT(Boss03HitboxRigComponent)


inline GameObject* Boss03HitboxRigComponent::WingBone(int wing) const
{
    GameObject* self = scene.Self();
    if (!self || wing < 0 || wing >= kBoss03WingCount) return nullptr;
    return FindInSubtree(*self, kBoss03WingBones[wing]);
}

inline void Boss03HitboxRigComponent::OnStart()
{
    for (EntityRef& ref : m_wings) ref = {};
    m_pending.clear();
    m_waited          = 0.0f;
    m_retryCooldown   = 0.0f;
    m_reported        = false;
    debugHitboxes     = 0;
    debugMissingBones = 0;

    if (!scene.Self()) return;
    Build();
}

inline void Boss03HitboxRigComponent::Build()
{
    const float scale = std::max(radiusScale, 0.01f);

    std::vector<Piece> pieces;
    pieces.push_back({ "Body", bodyRadius * scale, 0.0f, -1 });
    for (int i = 0; i < kBoss03WingCount; ++i)
        pieces.push_back({ kBoss03WingBones[i], wingRadius * scale,
                           std::max(wingLength, 0.05f), i });

    for (const Piece& piece : pieces) {
        if (BuildPiece(piece)) ++debugHitboxes;
        else                   m_pending.push_back(piece);
    }
    debugMissingBones = static_cast<int>(m_pending.size());
}

inline bool Boss03HitboxRigComponent::BuildPiece(const Piece& piece)
{
    /// @note 生成のたびに引き直す。scene.Create が GameObject 配列を再確保するので、
    ///       ループの外で掴んだポインタは 2 つ目以降で無効になる。
    GameObject* self = scene.Self();
    if (!self || !FindInSubtree(*self, piece.bone)) return false;

    const float radius = std::max(piece.radius, 0.01f);

    GameObject& hitbox = scene.Create("HB_" + piece.bone);
    /// @note シーンには保存しない。Play のたびにその時点のリグから組み直す。
    hitbox.runtimeGenerated = true;
    /// @note 部位もボス本体と同じ «敵» として扱わせる。
    hitbox.tag   = "Enemy";
    hitbox.layer = hitboxLayer & 31;

    GameObject* owner = scene.Self();
    if (GameObject* parent = owner ? FindInSubtree(*owner, piece.bone) : nullptr)
        hitbox.SetParent(*parent);

    if (piece.length > radius * 2.0f) {
        /// @note 骨のローカル +Y が骨の向き (Blender の primary_bone_axis='Y' で書き出している)。
        ///       根元から先端までを 1 本のカプセルで覆う。
        hitbox.transform.position = { 0.0f, piece.length * 0.5f, 0.0f };
        hitbox.transform.rotation = Quaternion::Identity();
        auto& collider = hitbox.AddComponent<CapsuleColliderComponent>();
        collider.SetCapsule(radius, std::max(piece.length * 0.5f - radius, 0.01f));
        collider.isTrigger = true;
    } else {
        hitbox.transform.position = Vector3::ZERO;
        auto& collider = hitbox.AddComponent<SphereColliderComponent>();
        collider.SetRadius(radius);
        collider.isTrigger = true;
    }

    if (piece.wing < 0) {
        auto& body = hitbox.AddScript<BossPartComponent>();
        body.bodyTarget = true;
        body.hitRadius = radius;
        body.bossOwner = EntityRef{scene.Self()->GetID()};
    }
    if (piece.wing >= 0) {
        auto& part = hitbox.AddScript<BossPartComponent>();
        /// @note 扇の判定はレンダラーを持たない部位に対して «本人の申告» を使う。
        part.hitRadius = radius;
        part.maxHealth = std::max(wingHealth, 1);
        m_wings[piece.wing] = EntityRef{ hitbox.GetID() };
    }
    return true;
}

inline void Boss03HitboxRigComponent::SyncDetached()
{
    const auto* anim = Anim();
    if (!anim) return;

    for (int i = 0; i < kBoss03WingCount; ++i) {
        GameObject* hitbox = m_wings[i].Resolve(scene);
        if (!hitbox) continue;
        const bool detached = anim->IsWingDetached(i);
        if (hitbox->activeSelf() == !detached) continue;

        hitbox->SetActive(!detached);

        auto* part = scene.GetScript<BossPartComponent>(hitbox);
        if (!part) continue;
        if (detached) {
            /// @note 落ちた翼はもう的ではない。畳むだけだと «斬れないのに輪郭が出る» が残る。
            part->Break();
        } else {
            /// @note 戻ってきた翼 (投げた翼) は的に戻す。壊れた印だけでは残り 0 のまま的に戻り 1 撃で再び壊れる扱いになるため、耐久ごと `Restore()` する。
            /// @note とどめで落とした翼はそもそも畳まれたままなので、ここへは来ない。
            if (part->IsBroken()) part->Restore();
        }
    }
}

inline void Boss03HitboxRigComponent::SyncFlinch()
{
    auto* anim = Anim();
    if (!anim) return;

    for (int i = 0; i < kBoss03WingCount; ++i) {
        GameObject* hitbox = m_wings[i].Resolve(scene);
        const auto* part = hitbox ? scene.GetScript<BossPartComponent>(hitbox) : nullptr;
        const float flash = part ? part->DamageFlash() : 0.0f;
        const float previous = m_flash[i];
        m_flash[i] = flash;

        /// @note 立ち上がりだけを «1 発» として拾う。«光っているか» で見ると、閃光が
        ///       減っていく途中のフレームでも斬られたと読んで、仰け反りが連続で鳴る。
        if (previous > 0.0f || flash <= 0.0f) continue;

        /// @note 軽い重みに留める理由 (`BossAnimatorComponent` の Hit Layer): 斬撃は 1 セットで 5 回当たるため毎回満額だと «痙攣» に見える。深い仰け反りは弾き返した締めの取り分。
        anim->ReactToHit(0.0f);
    }
}

inline void Boss03HitboxRigComponent::OnUpdate()
{
    SyncDetached();
    SyncFlinch();

    if (m_pending.empty()) return;

    /// @note `BuildPiece` は 1 つごとにサブツリーを再帰で歩く。骨がまだ無いのは正常な状態なので、揃うまで毎フレーム試さずクールダウンを挟む。
    m_waited        += std::max(Time::deltaTime, 0.0f);
    m_retryCooldown -= std::max(Time::deltaTime, 0.0f);
    if (m_retryCooldown > 0.0f) return;
    m_retryCooldown = kRetryInterval;

    for (std::size_t i = m_pending.size(); i-- > 0; ) {
        if (!BuildPiece(m_pending[i])) continue;
        ++debugHitboxes;
        m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(i));
    }
    debugMissingBones = static_cast<int>(m_pending.size());
    if (m_pending.empty()) return;

    /// @note 綴り違いは «その翼だけ斬れない» としか出ないため、名指しでログに言う。
    /// @note 起動直後はまだ骨が無いのが正常なので即座には言わず、`kGiveUpSeconds` 待ってから報告する。
    if (m_waited > kGiveUpSeconds && !m_reported) {
        m_reported = true;
        debug.LogError("Boss03HitboxRigComponent could not find " +
                       std::to_string(m_pending.size()) +
                       " bone(s) after " + std::to_string(static_cast<int>(kGiveUpSeconds)) +
                       "s. Check the rig names against Assets/Models/Boss_03/ExportManifest.json.");
        /// @note ここまで来たら «まだ出来ていない» ではなく «名前が違う»。探索の負荷だけが残る。
        m_pending.clear();
        debugMissingBones = 0;
    }
}

} // namespace sandbox
