// DemoGame
// GameVocab.hpp | sandbox
// ゲーム共通語彙 (アニメ状態名 / パラメータ名 / タグ) と共有判定ヘルパー。
//
// 設計意図 (WHY):
//   "Slash_01" などの魔法文字列が Player / Enemy / SwordTrail / WeaponHitbox /
//   SwordParticle の 5 スクリプトに散在し、タイプミスは黙って「常に false」になっていた。
//   ここに 1 箇所集約することで、状態追加は 1 箇所修正で済み、誤字は constexpr で
//   コンパイルエラー化する。補完も効く。
#pragma once
#include <Engine/Scene/ScriptProxy/ScriptAnimatorProxy.hpp>
#include <string_view>

namespace fbzz::scene { class GameObject; }

namespace sandbox {

using fbzz::scene::GameObject;
using fbzz::scene::ScriptAnimatorProxy;

// ── タグ ─────────────────────────────────────────────────────────────────────
namespace Tags {
    inline constexpr const char* Player = "Player";
    inline constexpr const char* Enemy  = "Enemy";
}

// ── アニメーター状態名 ───────────────────────────────────────────────────────
namespace AnimState {
    inline constexpr const char* Slash01     = "Slash_01";
    inline constexpr const char* Slash02     = "Slash_02";
    inline constexpr const char* Slash03     = "Slash_03";
    inline constexpr const char* CrouchSlash = "CrouchSlash";
    inline constexpr const char* CrouchIdle  = "CrouchIdle";
    inline constexpr const char* Block       = "Block";
    inline constexpr const char* Land        = "Land";
    inline constexpr const char* PlayerImpact = "PlayerImpact";
    inline constexpr const char* PlayerHit    = "PlayerHit";
    inline constexpr const char* Death        = "Death";
}

// ── アニメーターパラメータ名 ─────────────────────────────────────────────────
namespace AnimParam {
    inline constexpr const char* Speed         = "Speed";
    inline constexpr const char* VerticalSpeed = "VerticalSpeed";
    inline constexpr const char* IsGrounded    = "IsGrounded";
    inline constexpr const char* Jump          = "Jump";
    inline constexpr const char* Block         = "Block";
    inline constexpr const char* Crouch        = "Crouch";
    inline constexpr const char* Attack01      = "Attack01";
    inline constexpr const char* Attack02      = "Attack02";
    inline constexpr const char* Attack03      = "Attack03";
    inline constexpr const char* CrouchAttack  = "CrouchAttack";
    inline constexpr const char* Impact        = "Impact";
    inline constexpr const char* Hit           = "Hit";
    inline constexpr const char* Death         = "Death";
}

// ── 共有判定ヘルパー ─────────────────────────────────────────────────────────
// ステート名が通常コンボ Slash 群のいずれかか (文字列単体での判定)。
inline bool IsSlashStateName(std::string_view name)
{
    return name == AnimState::Slash01 || name == AnimState::Slash02 ||
           name == AnimState::Slash03 || name == AnimState::CrouchSlash;
}

// 剣を振っている通常コンボ State 群のいずれかか。go==nullptr なら自身を見る。
inline bool IsSlashState(const ScriptAnimatorProxy& animator, GameObject* go = nullptr)
{
    if (go) {
        return animator.IsInState(go, AnimState::Slash01) ||
               animator.IsInState(go, AnimState::Slash02) ||
               animator.IsInState(go, AnimState::Slash03) ||
               animator.IsInState(go, AnimState::CrouchSlash);
    }
    return animator.IsInState(AnimState::Slash01) ||
           animator.IsInState(AnimState::Slash02) ||
           animator.IsInState(AnimState::Slash03) ||
           animator.IsInState(AnimState::CrouchSlash);
}

// 攻撃モーションの「振り区間」(正規化時間 start〜end) に入っているか。
// WHY: 剣筋・血しぶき・ヒット判定はモーション全体ではなく実際に振り切る区間だけで成立させる。
//      コンボの Slash→Slash クロスフェード中は currentState が前段のままで、その正規化時間も
//      前段のものになるため、次段の振りの瞬間に窓がズレてしまう。そこで「現ステートの窓」に加え
//      「遷移先 Slash の窓 (blendToTime ベース)」も評価し、各段が自分の start〜end で判定されるようにする。
inline bool IsAttackSwing(const ScriptAnimatorProxy& animator, GameObject* go,
                          float start, float end)
{
    // 1) 現ステートが Slash で、その正規化時間が窓内
    if (IsSlashState(animator, go)) {
        const float t = go ? animator.GetNormalizedTime(go) : animator.GetNormalizedTime();
        if (t >= start && t <= end) return true;
    }
    // 2) コンボのクロスフェードで次の Slash へ遷移中なら、遷移先 Slash 自身の正規化時間で窓判定
    const std::string next = go ? animator.GetBlendToState(go) : animator.GetBlendToState();
    if (IsSlashStateName(next)) {
        const float tn = go ? animator.GetBlendToNormalizedTime(go)
                            : animator.GetBlendToNormalizedTime();
        if (tn >= start && tn <= end) return true;
    }
    return false;
}

} // namespace sandbox
