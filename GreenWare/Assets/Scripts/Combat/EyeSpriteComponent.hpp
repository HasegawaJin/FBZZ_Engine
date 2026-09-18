/// @file    EyeSpriteComponent.hpp
/// @brief   目のスプライトを差し替えて、キャラクターの表情を戦闘の出来事へ追従させる
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// @note テクスチャ差し替え (ボーン/モーフでなく): 専用ボーンだと全クリップにまぶたキーが
///       要る。絵の差し替えなら 1 スクリプトで完結しアニメーションと独立に間隔を持てる。
/// @note 受け取るのは「出来事」(CharacterEvent) で「顔」ではない: どのコマを当てるかは
///       Inspector 側の対応表に残し、敵とプレイヤーで同じ出来事に別の絵を当てられる。
///       保持秒数を出来事ごとに持ち、0 は「次の出来事まで続く」の意味にする。
/// @note キャラクターの根に登録する: 目は submesh 子に付くが CombatManager が知るのは根の
///       GameObject のため。瞬きは半透明ブレンドでなく即時カット (混ぜると二重に見える)。
#pragma once

#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/CharacterEvent.hpp>
#include <Scripts/Combat/IDamageable.hpp>
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EyeSpriteComponent : public Script {
    FBZZ_SCRIPT(EyeSpriteComponent)

    /// @brief 差し替え先の Material が要る。
    /// @note OPTIONAL にする理由: eyeRenderer で別 GameObject を指すと Material はそちらに
    ///       付くため。自分に必須と宣言すると正しい構成が「不足」警告され続ける
    ///       (解決できない場合は OnStart が実行時エラーで報告する)。
    FBZZ_OPTIONAL_COMPONENT(MaterialComponent)

public:
    /// @note Idle 以外は未設定を許す: 敵はまだ目のアトラスを持たず、全部必須だと瞬きだけ
    ///       入れたいキャラにも 6 枚を強いる。未設定の出来事は「反応しない」だけで済む。
    FBZZ_GROUP("Sprites")
    FBZZ_ASSET_FIELD(SpriteRef, idleSprite, "待機")
    FBZZ_TOOLTIP("通常時の目。アトラス内の 1 コマを指定してよい")
    FBZZ_ASSET_FIELD(SpriteRef, blinkSprite, "Blink")
    FBZZ_TOOLTIP("瞬きの瞬間だけ表示する閉じ目。未設定なら瞬かない")
    FBZZ_ASSET_FIELD(SpriteRef, spottedSprite, "Spotted")
    FBZZ_TOOLTIP("相手を見つけたときの顔")
    FBZZ_ASSET_FIELD(SpriteRef, attackSprite, "攻撃")
    FBZZ_TOOLTIP("攻撃を出したときの顔")
    FBZZ_ASSET_FIELD(SpriteRef, hurtSprite, "Hurt")
    FBZZ_TOOLTIP("ダメージを受けたときの顔")
    FBZZ_ASSET_FIELD(SpriteRef, deadSprite, "Dead")
    FBZZ_TOOLTIP("倒れたときの顔。以後この目は動かない")

    FBZZ_GROUP("Target Material")
    /// @note 参照で持たせる理由: 通常は同じ GameObject にマテリアルが並ぶが、制御スクリプトを
    ///       ルートへ集める構成では描画が子の GameObject になる。両対応のため既定は自分。
    FBZZ_REF(GameObject, eyeRenderer, "Eye Renderer")
    FBZZ_TOOLTIP("目のマテリアルを持つ GameObject。空なら自分自身。先頭の Material を使用")

    FBZZ_GROUP("保持")
    FBZZ_FIELD_RANGE(float, spottedHold, 0.8f, "Spotted Seconds", 0.0f, 6.0f)
    FBZZ_TOOLTIP("0 にすると、次の出来事まで続く顔になる (見つけている間ずっと睨む)")
    FBZZ_FIELD_RANGE(float, attackHold, 0.3f, "Attack Seconds", 0.0f, 6.0f)
    FBZZ_TOOLTIP("同上。攻撃の顔は動作より短く切ると、連射で貼り付いた顔にならない")
    FBZZ_FIELD_RANGE(float, hurtHold, 0.55f, "Hurt Seconds", 0.0f, 6.0f)

    FBZZ_GROUP("Blink")
    FBZZ_FIELD_RANGE(float, intervalMin, 2.8f, "間隔の下限", 0.2f, 15.0f)
    FBZZ_TOOLTIP("瞬きから次の瞬きまでの最短秒数")
    FBZZ_FIELD_RANGE(float, intervalMax, 6.0f, "間隔の上限", 0.2f, 15.0f)
    FBZZ_TOOLTIP("同・最長秒数。Min より小さくても自動で入れ替える")
    FBZZ_FIELD_RANGE(float, closedDuration, 0.09f, "Closed Duration", 0.02f, 0.6f)
    FBZZ_TOOLTIP("目を閉じている秒数。0.1 秒前後より長いと眠そうに見える")
    FBZZ_FIELD_RANGE_INT(int, doubleBlinkPercent, 25, "Double Blink %", 0, 100)
    FBZZ_TOOLTIP("2 回続けて瞬く確率")
    FBZZ_FIELD_RANGE(float, doubleBlinkGap, 0.11f, "Double Blink Gap", 0.02f, 0.5f)
    FBZZ_TOOLTIP("2 連の 1 回目と 2 回目の間で目を開けている秒数")

    FBZZ_GROUP("デバッグ")
    /// @note 文字列にする理由: IReflector::Readonly に bool 版が無く int (0/1) に落ちるため。
    ///       今どの顔かは状態名をそのまま出す方が読める。
    FBZZ_FIELD_READ_ONLY(std::string, debugFace, "Idle", "Face")
    FBZZ_FIELD_READ_ONLY(std::string, debugOwner, "", "Owner")
    FBZZ_TOOLTIP("この目が反応するキャラクター。CombatManager はこの名前へ出来事を配る")
    FBZZ_FIELD_READ_ONLY(float, debugTimer, 0.0f, "Phase Remaining")

    /// @name 外から呼ぶ入口
    /// @{

    /// character の目。character にはキャラクターの根 (殴られる側) を渡す。
    /// 目を持たないキャラクターでは nullptr が返る。
    [[nodiscard]] static EyeSpriteComponent* For(GameObject* character);

    /// シーンに居る目の数。「全キャラぶん揃っているか」をマネージャー側から見るため。
    [[nodiscard]] static int RegisteredCount() { return static_cast<int>(s_all.size()); }

    /// 出来事を 1 つ受け取り、対応する顔へ切り替える。
    /// 対応するスプライトが未設定の出来事は何も起こさない (Defeated を除く)。
    void React(CharacterEvent event);

    /// 今すぐ 1 回瞬く。驚きなどの軽い反応に使う。瞬きの途中なら閉じ時間をやり直す。
    void Blink();

    /// 瞬きと表情を止める / 再開する。カットシーンで目を別のスクリプトが握るとき用。
    void SetSuppressed(bool suppressed);

    /// 倒れた顔も含めて通常時へ戻す。リスポーンやリトライから呼ぶ。
    void ResetFace();

    [[nodiscard]] bool IsDefeated() const { return m_base == EyeFace::Dead; }

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;
    /// @}

private:
    /// 表示しうる絵の一覧。CharacterEvent と 1 対 1 ではない (瞬きは出来事ではない)。
    enum class EyeFace { Idle, Blink, Spotted, Attack, Hurt, Dead, Count };

    static inline std::vector<EyeSpriteComponent*> s_all;

    [[nodiscard]] static EyeFace FaceFor(CharacterEvent event);
    [[nodiscard]] static int     Priority(EyeFace face);
    [[nodiscard]] static const char* NameOf(EyeFace face);

    [[nodiscard]] float HoldFor(EyeFace face) const;
    [[nodiscard]] const TextureRef& TextureFor(EyeFace face) const
    {
        return m_textures[static_cast<std::size_t>(face)];
    }
    [[nodiscard]] bool CanBlink() const;

    /// 続く顔を変える。瞬きが走るのは Idle のときだけ。
    void SetBase(EyeFace face);
    /// 一時的な顔を seconds 秒だけ被せる。
    void ShowOverlay(EyeFace face, float seconds);
    /// 今の状態から 1 枚選んで material へ書く。
    void Apply();
    void UpdateBlink(float dt);

    [[nodiscard]] float RollNextInterval();
    [[nodiscard]] MaterialInstance EyeMaterial() const;
    /// 反応の宛先になるキャラクターの根を探す。
    [[nodiscard]] GameObject* ResolveOwner() const;

    /// Sprite 参照をテクスチャ差し替え用の型へ移し替えたもの。EyeFace の並びと 1 対 1。
    /// @note OnStart で作って持つ理由: 解決は GUID → パスの逆引きを伴うため、
    ///       表情切り替えのたびに引き直す理由がない。
    TextureRef m_textures[static_cast<std::size_t>(EyeFace::Count)];

    /// 反応の宛先。CombatManager が渡してくる GameObject と突き合わせる鍵になる。
    EntityID m_owner{};

    EyeFace m_base    = EyeFace::Idle;
    EyeFace m_overlay = EyeFace::Idle;
    float   m_overlayTimer = 0.0f;

    /// 瞬きのフェーズ。開いている間は次の瞬きまで、閉じている間は開くまで、
    /// 2 連の合間は 2 回目までの残り時間を表す。
    float m_blinkTimer  = 0.0f;
    bool  m_blinkClosed = false;
    /// この瞬きであと何回閉じるか。2 なら 2 連。開いた瞬間に 1 減らす。
    int   m_remainingBlinks = 1;

    bool    m_suppressed = false;
    /// 材質へ 1 度も書けていないか。OnStart で通常時の目を必ず 1 回書き込むためのフラグ。
    bool    m_hasApplied = false;
    EyeFace m_appliedFace = EyeFace::Idle;
};

FBZZ_REFLECT(EyeSpriteComponent)


/// 目のスプライトを差し込む Material のテクスチャスロット。
///
/// @note "t0" と書く理由: SetTexture はシェーダーの変数名 (texAlbedo) で検証するため
///       "albedo" は不一致で静かに捨てられる。"t0" はレジスタ番号として解釈され、
///       スロット一致で正規名 "albedo" のキーへ格納される。
inline constexpr MaterialPropertyId kAlbedoTextureId{ "t0" };

inline EyeSpriteComponent* EyeSpriteComponent::For(GameObject* character)
{
    if (!character) return nullptr;

    const EntityID id = character->GetID();
    for (EyeSpriteComponent* eyes : s_all)
        if (eyes && eyes->m_owner == id) return eyes;
    return nullptr;
}

inline EyeSpriteComponent::EyeFace EyeSpriteComponent::FaceFor(CharacterEvent event)
{
    switch (event) {
    case CharacterEvent::Spotted:  return EyeFace::Spotted;
    case CharacterEvent::Attack:   return EyeFace::Attack;
    case CharacterEvent::Hurt:     return EyeFace::Hurt;
    case CharacterEvent::Defeated: return EyeFace::Dead;
    case CharacterEvent::Recovered: break;
    }
    return EyeFace::Idle;
}

/// 一時的な顔どうしがぶつかったとき、どちらを残すか。
///
/// @note 新しい方を無条件に採らない理由: 被弾直後に攻撃が通ると痛がる顔が 1 フレームで
///       攻撃顔に差し替わり被弾自体が読めなくなるため、被弾は自分の操作より優先する。
inline int EyeSpriteComponent::Priority(EyeFace face)
{
    switch (face) {
    case EyeFace::Hurt:    return 3;
    case EyeFace::Attack:  return 2;
    case EyeFace::Spotted: return 1;
    default: break;
    }
    return 0;
}

inline const char* EyeSpriteComponent::NameOf(EyeFace face)
{
    switch (face) {
    case EyeFace::Blink:   return "Blink";
    case EyeFace::Spotted: return "Spotted";
    case EyeFace::Attack:  return "Attack";
    case EyeFace::Hurt:    return "Hurt";
    case EyeFace::Dead:    return "Dead";
    default: break;
    }
    return "Idle";
}

inline float EyeSpriteComponent::HoldFor(EyeFace face) const
{
    switch (face) {
    case EyeFace::Spotted: return Max(spottedHold, 0.0f);
    case EyeFace::Attack:  return Max(attackHold, 0.0f);
    case EyeFace::Hurt:    return Max(hurtHold, 0.0f);
    default: break;
    }
    return 0.0f;
}

inline MaterialInstance EyeSpriteComponent::EyeMaterial() const
{
    /// @note eyeRenderer 未アサインなら EntityRef が無効になり、Instance が自分自身へ落ちる。
    ///       MaterialInstance は指定先 GameObject の先頭 Material を使用する。index を持たない
    ///       理由: 瞳専用 Renderer は Material を 1 つだけ持つ構成に統一し、submesh 配列変更
    ///       による番号ずれを無くす。
    return material.Instance(eyeRenderer.ref);
}

inline GameObject* EyeSpriteComponent::ResolveOwner() const
{
    /// @note 「殴られる側」= IDamageable を持つ GameObject をキャラクターの根とみなす。名前や
    ///       tag で探さない理由: プレイヤーと敵で規則が変わるため。CombatManager がダメージを
    ///       入れる相手そのものを鍵にすれば宛先の食い違いが起こりようがない。
    for (GameObject* object = scene.Self(); object; object = object->GetParent())
        if (scene.GetScript<IDamageable>(object)) return object;
    return scene.Self();
}

inline bool EyeSpriteComponent::CanBlink() const
{
    return !m_suppressed && m_base == EyeFace::Idle && m_overlayTimer <= 0.0f &&
           TextureFor(EyeFace::Blink).IsValid();
}

inline void EyeSpriteComponent::OnStart()
{
    /// @note Sprite 参照 (guid + アトラス内のコマ) をそのまま引き継ぐ。ResolvePath() を経由
    ///       しない理由: 文字列へ落とすと GUID が失われ、画像を移動した後に参照が切れる。
    m_textures[static_cast<std::size_t>(EyeFace::Idle)].reference    = idleSprite.reference;
    m_textures[static_cast<std::size_t>(EyeFace::Blink)].reference   = blinkSprite.reference;
    m_textures[static_cast<std::size_t>(EyeFace::Spotted)].reference = spottedSprite.reference;
    m_textures[static_cast<std::size_t>(EyeFace::Attack)].reference  = attackSprite.reference;
    m_textures[static_cast<std::size_t>(EyeFace::Hurt)].reference    = hurtSprite.reference;
    m_textures[static_cast<std::size_t>(EyeFace::Dead)].reference    = deadSprite.reference;

    if (!idleSprite.IsValid()) {
        /// @note 通常時の 1 枚が無いと、瞬きも表情も「戻る先」を失う。ここで必ず声を上げる。
        debug.LogError("EyeSpriteComponent requires an Idle sprite (every other face "
                       "returns to it).");
    }
    if (!EyeMaterial().IsValid()) {
        debug.LogError("EyeSpriteComponent found no Material to write to "
                       "(check Eye Renderer and its first Material).");
    }

    GameObject* owner = ResolveOwner();
    m_owner    = owner ? owner->GetID() : EntityID{};
    debugOwner = owner ? owner->name : std::string{};

    if (std::find(s_all.begin(), s_all.end(), this) == s_all.end())
        s_all.push_back(this);

    m_suppressed = false;
    ResetFace();
}

inline void EyeSpriteComponent::OnDestroy()
{
    s_all.erase(std::remove(s_all.begin(), s_all.end(), this), s_all.end());
}

inline void EyeSpriteComponent::ResetFace()
{
    m_base         = EyeFace::Idle;
    m_overlayTimer = 0.0f;
    m_blinkClosed  = false;
    /// @note RollNextInterval() が m_remainingBlinks も引き直す。
    m_blinkTimer   = RollNextInterval();
    m_hasApplied   = false;
    Apply();
}

inline float EyeSpriteComponent::RollNextInterval()
{
    /// @note 2 連にするかを「次の瞬き」の抽選と同時に決める。閉じる直前に決めると、Blink() で
    ///       割り込んだ回まで 2 連の抽選対象になり、反応の瞬きが勝手に 2 回出る。
    m_remainingBlinks = random.Chance(static_cast<float>(doubleBlinkPercent) * 0.01f) ? 2 : 1;

    /// @note Inspector で Min > Max に設定されても止まらないよう、ここで並べ直す。
    const float low  = Min(intervalMin, intervalMax);
    const float high = Max(intervalMin, intervalMax);
    return random.Range(low, high);
}

inline void EyeSpriteComponent::Apply()
{
    EyeFace face = m_base;
    if (m_overlayTimer > 0.0f)                            face = m_overlay;
    else if (m_base == EyeFace::Idle && m_blinkClosed)    face = EyeFace::Blink;

    /// @note 論理状態は材質が引けたかに関わらず必ず進める: 書き込み失敗で「今の顔」を据え置くと
    ///       状態機械が古い顔のまま判断を続け瞬きの周期が壊れる。絵が出ないことと時間の
    ///       進行は分けて扱う。
    const bool changed = !m_hasApplied || m_appliedFace != face;
    m_appliedFace = face;
    debugFace     = NameOf(face);

    /// @note 状態が変わっていないなら書かない。SetTexture は検証とマップ書き込みを伴い、
    ///       毎フレーム呼ぶと数秒に 1 回で足りる処理を 100 倍以上走らせることになる。
    if (!changed) return;

    /// @note 未設定の顔では書かずに今の絵を残す。Dead だけは絵が無くても状態を進めるので、
    ///       「倒れたのに瞬き続ける」ではなく「最後の顔で止まる」になる。
    const TextureRef& texture = TextureFor(face);
    if (!texture.IsValid()) return;

    const MaterialInstance instance = EyeMaterial();
    /// @note 解決できなかったときは m_hasApplied を立てない。次に顔が変わったとき再試行され、
    ///       材質が後から揃った場合に自動で復帰する。
    if (!instance.IsValid()) return;

    instance.SetTexture(kAlbedoTextureId, texture);
    m_hasApplied = true;
}

inline void EyeSpriteComponent::SetBase(EyeFace face)
{
    m_base         = face;
    m_overlayTimer = 0.0f;
    if (face == EyeFace::Idle) {
        m_blinkClosed = false;
        m_blinkTimer  = RollNextInterval();
    }
    Apply();
}

inline void EyeSpriteComponent::ShowOverlay(EyeFace face, float seconds)
{
    /// @note 走っている顔より弱い出来事では上書きしない。
    if (m_overlayTimer > 0.0f && Priority(face) < Priority(m_overlay)) return;

    m_overlay      = face;
    m_overlayTimer = seconds;
    /// @note 被せている間は瞬きを止める。開いた目に戻さないと、閉じ目のまま表情へ移った場合に
    ///       「閉じたまま驚いている」ように見える。
    m_blinkClosed  = false;
    Apply();
}

inline void EyeSpriteComponent::React(CharacterEvent event)
{
    if (m_suppressed) return;

    const EyeFace face = FaceFor(event);

    /// @note 倒れた顔は ResetFace() まで動かさない。倒れたまま瞬く / 驚くのは、
    ///       調整で選べてよい違いではない。
    if (face == EyeFace::Dead) {
        SetBase(EyeFace::Dead);
        return;
    }
    if (m_base == EyeFace::Dead) return;

    if (event == CharacterEvent::Recovered) {
        SetBase(EyeFace::Idle);
        return;
    }

    /// @note 割り当てのない出来事は、無反応であって無表情ではない。今の顔をそのまま続ける。
    if (!TextureFor(face).IsValid()) return;

    const float hold = HoldFor(face);
    if (hold <= 0.0f) SetBase(face);
    else              ShowOverlay(face, hold);
}

inline void EyeSpriteComponent::Blink()
{
    if (!CanBlink()) return;

    /// @note すでに閉じているなら閉じ時間をやり直すだけ。ここで再度閉じる扱いにすると
    ///       m_remainingBlinks が減らないまま閉じ直し、連打で目が開かなくなる。
    m_blinkClosed = true;
    m_blinkTimer  = closedDuration;
    Apply();
}

inline void EyeSpriteComponent::SetSuppressed(bool suppressed)
{
    if (m_suppressed == suppressed) return;
    m_suppressed = suppressed;
    /// @note 再開時は必ず開いた状態から数え直す。止めている間に閉じたまま固定されていた場合、
    ///       そのまま次の「開く」を待つと閉じ目のまま数秒放置される。
    if (!suppressed && m_base != EyeFace::Dead) SetBase(EyeFace::Idle);
}

inline void EyeSpriteComponent::UpdateBlink(float dt)
{
    if (!CanBlink()) {
        debugTimer = 0.0f;
        return;
    }

    m_blinkTimer -= dt;
    debugTimer = Max(0.0f, m_blinkTimer);
    if (m_blinkTimer > 0.0f) return;

    if (m_blinkClosed) {
        /// @note 閉じ終わり。開けてから、2 連の残りがあるかで次の待ち時間を決める。
        m_blinkClosed = false;
        m_remainingBlinks -= 1;
        m_blinkTimer = (m_remainingBlinks > 0) ? doubleBlinkGap : RollNextInterval();
    } else {
        m_blinkClosed = true;
        m_blinkTimer  = closedDuration;
    }
    Apply();
}

inline void EyeSpriteComponent::OnUpdate()
{
    if (m_suppressed) {
        debugTimer = 0.0f;
        return;
    }

    const float dt = Time::deltaTime;

    if (m_overlayTimer > 0.0f) {
        m_overlayTimer = Max(0.0f, m_overlayTimer - dt);
        debugTimer     = m_overlayTimer;
        /// @note 被せていた顔が切れた瞬間に、続く顔 (通常は Idle) へ戻す。
        if (m_overlayTimer <= 0.0f) {
            m_blinkTimer = RollNextInterval();
            Apply();
        }
        return;
    }

    UpdateBlink(dt);
}

} // namespace sandbox
