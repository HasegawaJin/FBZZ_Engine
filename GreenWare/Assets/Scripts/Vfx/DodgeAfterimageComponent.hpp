/// @file    DodgeAfterimageComponent.hpp
/// @brief   回避中のプレイヤーに残像を出す。無敵時間を «体そのもの» で伝える層
/// @author  Hasegawa Jin
/// @date    2026-09-06
///
/// シーンへは付けない。PlayerComponent が内部モジュールとして持つ (他の Player
/// モジュールと同じ形)。回避の発生は PlayerControllerComponent へ問い合わせるだけで、
/// 挙動には手を出さない ─ 丸ごと外しても «避ける» は成立する。
/// @note MeshTrailComponent (過去のボーン姿勢の再描画) を使う。パーティクルは体の形を
///       持たず «すり抜けた» 感が出ない。色は極性でなくプレイヤー色 (緑、画面の縁
///       Surge と同じ語彙)。刀も残す (輪郭の移動量が速さの手がかりになるため)。
/// @note 無敵時間と厳密に同期する。判定は PlayerComponent::TryPerfectDodge が
///       m_controller.IsDodging() を見るのが正本で、残像も同じ述語から出す
///       (秒数を別に持つと «消えているのに当たらない» が生まれる)。
#pragma once

#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class DodgeAfterimageComponent : public Script {
    FBZZ_SCRIPT(DodgeAfterimageComponent)

public:
    /// @note フィールド名に ghost/flux を付ける。PlayerComponent は全モジュールの Reflect
    ///       を 1 つの名前空間へ平らに並べ、PlayerControllerComponent が既に
    ///       dodgeCameraKick/dodgeBufferSeconds を出しているため「回避の」だけでは足りない。
    FBZZ_GROUP("Dodge Afterimage")
    FBZZ_FIELD_COLOR(ghostColor, (Vector4{ kColorPlayer.x, kColorPlayer.y,
                                           kColorPlayer.z, 0.55f }), "Ghost Color")
    FBZZ_TOOLTIP("残像の色。アルファが «いちばん新しい 1 枚» の濃さ。"
                 "極性色は乗せない ─ 回避はどちらの剣とも関係が無い")
    FBZZ_FIELD_RANGE(float, ghostTailAlpha, 0.0f, "Tail Alpha", 0.0f, 1.0f)
    FBZZ_TOOLTIP("いちばん古い 1 枚の濃さ。0 で消え際まで薄くなる")

    FBZZ_FIELD_RANGE(float, ghostFade, 0.30f, "フェード", 0.05f, 1.5f)
    FBZZ_TOOLTIP("残像 1 枚が消えるまで [秒]。回避 (0.24 秒) より少し長くすると、"
                 "抜け切った後に «居た跡» が一拍だけ残る")
    FBZZ_FIELD_RANGE(float, ghostInterval, 1.0f / 30.0f, "間隔", 1.0f / 120.0f, 0.2f)
    FBZZ_TOOLTIP("残像を 1 枚置く間隔 [秒]。短くするほど濃い帯になるが、"
                 "そのぶんスキニングの描画が増える")
    FBZZ_FIELD_RANGE_INT(int, ghostMaxSamples, 6, "Max Ghosts", 1, 16)
    FBZZ_TOOLTIP("同時に出す枚数の上限。プレイヤーは部位ごとに 10 個のメッシュへ"
                 "分かれているので、1 枚増やすたびに 10 回ぶん描画が増える")
    FBZZ_FIELD_RANGE(float, ghostMinStep, 0.04f, "最小の刻み", 0.0f, 0.5f)
    FBZZ_TOOLTIP("これ以上動いたときだけ 1 枚置く [m]。0 で間隔だけで置く。"
                 "その場で回るだけの回避が «団子» にならないための下限")
    FBZZ_FIELD(bool, ghostIncludeBlades, true, "Include Blades")
    FBZZ_TOOLTIP("双剣にも残像を出す。切ると転がりが実際より鈍く見える")
    /// @note 一定の濃さでは引かない。回避は踏み切りが最速で抜け際は Dodge End Speed x
    ///       まで落ちるため、同じ濃さ・間隔だと転がり全体が «一定速度» に均される。
    ///       濃さと刻みを速度カーブへ乗せると減速が絵から読める。
    FBZZ_FIELD_RANGE(float, ghostLead, 0.7f, "出だしの上乗せ", 0.0f, 2.0f)
    FBZZ_TOOLTIP("踏み切りの瞬間に濃さと枚数をどれだけ増すか。0 で回避のあいだ一定。"
                 "抜け際は Ghost Color そのままの濃さへ戻る")
    /// @note 専用材質を張る。組み込みの残像シェーダーは形の中まで同じ濃さで塗り、
    ///       重なった枚数ぶん濃くなって «団子» になる。PlayerGhost.hlsl は手前を向いた
    ///       面を抜いて輪郭だけを残す。色は材質に持たせない (Ghost/Perfect Color は
    ///       1 回ごとに変わる値なので、材質が持てるのは «形» だけ)。
    FBZZ_FIELD_FILE(ghostMaterial, "Assets/Materials/Effects/M_PlayerGhost.mat",
                    "Ghost Material", ".mat")
    FBZZ_TOOLTIP("残像の材質 (render_path = \"trail\")。空にすると組み込みの塗り潰しに戻る")

    /// @name ジャスト回避
    /// @{
    /// @note 別の «技» にしない。ジャスト回避は «同じ回避が報われた» だけで、同じ残像が
    ///       その場で明るく長くなる方が «今のが良かった» を 1 つの絵で伝えられる。
    FBZZ_GROUP("Dodge Afterimage — ジャスト回避")
    FBZZ_FIELD_COLOR(fluxColor, (Vector4{ 0.75f, 1.00f, 0.85f, 0.95f }), "Perfect Color")
    FBZZ_TOOLTIP("かわした瞬間に残像が塗り替わる色。画面の縁 (Surge) と同じ語彙で"
                 "白熱させる ─ 極の赤青とも被弾の赤とも取り違えない")
    FBZZ_FIELD_RANGE(float, fluxFade, 0.55f, "Perfect Fade", 0.05f, 2.0f)
    FBZZ_TOOLTIP("そのときだけ残像が残る長さ [秒]。通常より長く引くことで、"
                 "スロー (perfectDodgeSlowSeconds) の間ずっと «避けた形» が見える")

    FBZZ_FIELD_READ_ONLY(int, debugGhostMeshes, 0, "Ghost Meshes")

    /// 回避の «出た / 終わった» はここから読む。注入されないと何も出さない。
    void SetController(PlayerControllerComponent* controller) { m_controller = controller; }

    /// ジャスト回避。今出ている残像をその場で白熱させ、長く引く。
    /// @note 新しく出し直さない。この瞬間に居るのは «かわした姿勢» で既に残像として
    ///       並んでいる。作り直すといちばん見せたい «潜り抜けた形» が消える。
    void Flash();

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;
    /// @}

private:
    /// 残像を出す 1 メッシュぶん。実体は Player の子 (部位) と双剣。
    struct Ghost {
        EntityRef ref;
        /// この実体へ MeshTrailComponent を足したのはこちらか。
        /// シーンが最初から持っていた個体を Play 終わりに消すと、作者が付けた設定ごと消える。
        bool owned = false;
    };

    /// 残像を出す実体を集め直す。刀は抜刀で後から現れうるので、毎回頭から数える。
    void Collect();
    /// 記録を始める / 止める。stop は «記録だけ止めて自然消滅を待つ»。
    void Begin();
    void Stop();
    /// 出ている残像すべてへ色と寿命を書く。Begin と Flash が同じ 1 本を通る。
    void Paint(const Vector4& head, float fade);
    /// 回避の進み具合に合わせて濃さと刻みを書き直す。回避中は毎フレーム通る。
    void Drive(float progress01);
    /// 足した MeshTrailComponent を畳む。付けていない個体には触らない。
    void Release();

    [[nodiscard]] MeshTrailComponent* TrailOf(const Ghost& ghost) const;

    PlayerControllerComponent* m_controller = nullptr;
    std::vector<Ghost>         m_ghosts;
    /// 直前に見た回避の番号。変わった瞬間が «出た» の 1 フレーム。
    int  m_lastSerial = 0;
    bool m_active     = false;
    /// ジャスト回避で塗り替えたか。塗ったあとに速度カーブで薄め直すと、
    /// «良かった» と言った次のフレームに自分でそれを取り消すことになる。
    bool m_flux       = false;
};

FBZZ_REFLECT(DodgeAfterimageComponent)


inline void DodgeAfterimageComponent::OnStart()
{
    /// @note 前回 Play / DLL リロードで足した枠は OnDestroy で畳まれている。畳めていない
    ///       経路が残っても、Collect が同じ実体を拾い直して上書きするだけで済む。
    m_ghosts.clear();
    m_lastSerial = m_controller ? m_controller->DodgeSerial() : 0;
    m_active     = false;
    m_flux       = false;
    debugGhostMeshes = 0;
}

inline void DodgeAfterimageComponent::OnDestroy()
{
    Release();
}

inline MeshTrailComponent* DodgeAfterimageComponent::TrailOf(const Ghost& ghost) const
{
    GameObject* object = ghost.ref.Resolve(scene);
    return object ? object->GetComponent<MeshTrailComponent>() : nullptr;
}

inline void DodgeAfterimageComponent::Collect()
{
    m_ghosts.clear();

    GameObject* self = scene.Self();
    if (!self) return;

    /// @note 部位を 1 つずつ拾う。プレイヤーは材質ごとに 10 個の SkinnedMeshRenderer へ
    ///       分かれ (P_ArmorWhite/P_GlowGreen…)、MeshTrailComponent は «その GameObject
    ///       の描画» だけを残すため、根に 1 つ付けても何も出ない。
    /// @note «数える» と «足す» を 2 周に分ける。AddComponent はシーンの配列を伸ばしうる
    ///       ので、1 周で回すと足した瞬間に前のポインタが別の実体を指してしまう。
    const auto note = [&](GameObject& object) {
        if (!object.GetComponent<SkinnedMeshRenderer>()) return;
        m_ghosts.push_back(Ghost{ EntityRef{ object.GetID() }, false });
    };

    const int childCount = self->GetChildCount();
    for (int i = 0; i < childCount; ++i)
        if (GameObject* child = self->GetChild(i)) note(*child);

    /// @note 刀は Player の子ではなくルートに置かれている (理由は WeaponRigComponent を参照)。
    ///       名前で拾うしかないので、抜刀で後から現れる場合に備えて毎回数え直す。
    if (ghostIncludeBlades) {
        const HandSide hands[] = { HandSide::Right };
        for (const HandSide hand : hands)
            if (GameObject* sword = scene.Find(SwordObjectName(hand)))
                note(*sword);
    }

    /// @note 2 周目。ここからは必ず ID から引き直す。
    for (Ghost& ghost : m_ghosts) {
        GameObject* object = ghost.ref.Resolve(scene);
        if (!object) continue;
        if (object->GetComponent<MeshTrailComponent>()) continue;
        ghost.owned = true;
        /// @note シーンへ保存させない。回避の残像は «実行中だけ» の状態で、
        ///       保存すると編集画面のプレイヤーが常時ぼやける。
        object->AddComponent<MeshTrailComponent>().enabled = false;
    }

    debugGhostMeshes = static_cast<int>(m_ghosts.size());
}

inline void DodgeAfterimageComponent::Paint(const Vector4& head, float fade)
{
    const Vector4 tail{ head.x, head.y, head.z, Clamp01(ghostTailAlpha) };
    for (const Ghost& ghost : m_ghosts) {
        MeshTrailComponent* trail = TrailOf(ghost);
        if (!trail) continue;
        trail->colorStart = head;
        trail->colorEnd   = tail;
        trail->duration   = Max(fade, 0.05f);
    }
}

inline void DodgeAfterimageComponent::Drive(float progress01)
{
    /// @note 出だしが最大で、抜け際に Ghost Color そのままへ戻る。速度の落ち方
    ///       (Dodge End Speed x) と同じ «線形に落ちる» 形をそのまま借りる。
    const float boost = 1.0f + Max(ghostLead, 0.0f) * (1.0f - Clamp01(progress01));

    Vector4 head = ghostColor;
    head.w = Clamp01(ghostColor.w * boost);
    Paint(head, ghostFade);

    /// @note 濃さだけ上げると «薄い帯が濃くなった» で終わる。刻みも同じ倍率で詰めると
    ///       枚数そのものが増え、速い区間だけ帯が «連続した面» になる。
    const float step = Max(ghostInterval, 1.0f / 120.0f) / boost;
    for (const Ghost& ghost : m_ghosts)
        if (MeshTrailComponent* trail = TrailOf(ghost))
            trail->sampleInterval = Max(step, 1.0f / 120.0f);
}

inline void DodgeAfterimageComponent::Begin()
{
    Collect();

    for (const Ghost& ghost : m_ghosts) {
        MeshTrailComponent* trail = TrailOf(ghost);
        if (!trail) continue;
        /// @note 材質は毎回書く。Play 中に差し替えても次の回避から効くようにする
        ///       (パスは .mat 単位で 1 回しか解決しないので、書き続けても重くない)。
        trail->materialPath   = ghostMaterial;
        trail->sampleInterval = Max(ghostInterval, 1.0f / 120.0f);
        trail->minVertexDist  = Max(ghostMinStep, 0.0f);
        trail->maxSamples     = std::clamp(ghostMaxSamples, 1, 16);
        /// @note 裏面も描く。転がりは体が丸まるので、片面だけだと «内側» が抜けて
        ///       残像が空洞に見える。
        trail->doubleSided    = true;
        /// @note 自然消滅させる。回避が終わった瞬間に残像が全部消えると «絵が急に
        ///       無くなった» と読まれるため、記録だけ止めて枚数は寿命で薄れさせる。
        trail->clearOnDisable = false;
        /// @note 前の回避の枚数を引き継ぐと、跳んだ先と前回の位置が 1 本に繋がって見える。
        trail->clearRequested = true;
        trail->lastSampleTime = -1.0f;
        trail->enabled        = true;
    }

    m_flux   = false;
    m_active = true;
    Drive(0.0f);
}

inline void DodgeAfterimageComponent::Stop()
{
    for (const Ghost& ghost : m_ghosts)
        if (MeshTrailComponent* trail = TrailOf(ghost))
            /// @note clearOnDisable = false なので寿命で薄れる
            trail->enabled = false;
    m_active = false;
}

inline void DodgeAfterimageComponent::Flash()
{
    /// @note 回避の «外» から呼ばれる。残像が 1 枚も無いフレーム (回避していない被弾) では
    ///       塗る相手が居ないので、そのまま何も起きない。
    m_flux = true;
    Paint(fluxColor, fluxFade);
}

inline void DodgeAfterimageComponent::Release()
{
    for (const Ghost& ghost : m_ghosts) {
        GameObject* object = ghost.ref.Resolve(scene);
        if (!object) continue;
        auto* trail = object->GetComponent<MeshTrailComponent>();
        if (!trail) continue;
        if (ghost.owned) {
            /// @note 足したのはこちら。samples の GPU 資源は «消す» 経路を通してから外す。
            trail->enabled        = false;
            trail->clearOnDisable = true;
            trail->clearRequested = true;
        } else {
            /// @note シーンが持っていた個体。作者の設定を壊さないよう、切るだけにする。
            trail->enabled = false;
        }
    }
    m_ghosts.clear();
    m_active = false;
    m_flux   = false;
    debugGhostMeshes = 0;
}

inline void DodgeAfterimageComponent::OnUpdate()
{
    if (!enabled || !m_controller) return;

    const bool dodging = m_controller->IsDodging();
    const int  serial  = m_controller->DodgeSerial();

    /// @note «出た» は番号で取る。IsDodging の立ち上がりだけだと、回避が途切れずに
    ///       連続したフレームで 1 回目と 2 回目の残像が 1 本に繋がる。
    if (dodging && serial != m_lastSerial) {
        m_lastSerial = serial;
        Begin();
        return;
    }

    /// @note 回避のあいだは速度カーブに合わせて濃さと刻みを書き直し続ける。
    ///       ジャスト回避で塗り替えた後は触らない ─ そこは «速さ» ではなく «報われた» の絵で、
    ///       減速に合わせて薄め直すと、良かったと言った次のフレームに自分で取り消すことになる。
    if (dodging && m_active && !m_flux) Drive(m_controller->DodgeProgress01());

    if (!dodging && m_active) Stop();
}

} // namespace sandbox
