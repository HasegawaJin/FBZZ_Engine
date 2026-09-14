/// @file    DodgeAfterimageComponent.hpp
/// @brief   回避中のプレイヤーに残像を出す。無敵時間を «体そのもの» で伝える層
/// @author  Hasegawa Jin
/// @date    2026-09-06
///
/// シーンへは付けない。PlayerComponent が内部モジュールとして持つ (他の Player
/// モジュールと同じ形)。回避したことは PlayerControllerComponent へ問い合わせるだけで、
/// こちらから回避の挙動を触ることは無い ─ 丸ごと外しても «避ける» は全部成立する。
///
/// WHY 足元の煙ではなく «体» に出すか:
///   回避の出だしには既に土煙 (PlayRunDust) とカメラの引き (Punch) が入っている。
///   どちらも «床と画面» に出る演出で、プレイヤーの体には何も起きていない。
///   避けているのは体なので、体が変わらないと «速く滑っただけ» に見える。
///
/// WHY 残像 (MeshTrail) か:
///   回避で伝えたいことは 2 つ ─ «速い» と «無敵» で、どちらも «そこに在ったはずの体が
///   もう無い» という 1 つの絵で言える。パーティクルを撒くと «速い» は出るが、
///   撒いた粒は体の形を持たないので «すり抜けた» にはならない。エンジンの
///   MeshTrailComponent は過去のボーン姿勢ごと再描画するので、残像は転がっている
///   途中の «そのポーズ» のまま残る。
///
/// WHY 無敵時間とぴったり重ねるか:
///   無敵の判定は PlayerComponent::TryPerfectDodge が m_controller.IsDodging() を
///   見ているのが正本。残像も同じ 1 つの述語から出す ─ 秒数を別に持つと、
///   «残像が消えているのに当たらない» / «残像が出ているのに食らう» が生まれる。
///   避けられる時間は画面から読めなければ意味が無い。
///
/// WHY 色を極性から取らないか:
///   回避はどちらの剣とも関係が無い。赤青を出すと «極が乗った» と読み違える。
///   プレイヤー色 (緑) は «自分に良いことが起きた» の語彙として画面の縁 (Surge) でも
///   使っている ─ ジャスト回避で両方が同時に光ると、1 つの出来事として繋がる。
///
/// WHY 刀も残すか:
///   残像の «速さ» は輪郭の移動量で読まれる。体だけ残すと、いちばん大きく振れている
///   2 本の刃が抜け落ちて、転がりが実際より鈍く見える。
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
    // WHY 名前に ghost / flux を付けるか: PlayerComponent は全モジュールの Reflect を
    //     1 つの名前空間へ平らに並べる。PlayerControllerComponent が既に dodgeCameraKick /
    //     dodgeBufferSeconds を出しているので、«回避の» という接頭辞だけでは足りない。
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
    // WHY 一定の濃さで引かないか:
    //   回避の速さは踏み切りが最大で、抜け際は Dodge End Speed x まで落ちる。
    //   残像を最初から最後まで同じ濃さ・同じ間隔で置くと、いちばん速い 0.1 秒と
    //   ほぼ止まっている 0.1 秒が同じ帯になり、転がり全体が «一定の速さで滑った»
    //   に均される。濃さと刻みを速度カーブへ乗せると、帯そのものが «弾けて、
    //   伸びて、収まる» 形を持つ ─ 減速は絵から読めるようになる。
    FBZZ_FIELD_RANGE(float, ghostLead, 0.7f, "出だしの上乗せ", 0.0f, 2.0f)
    FBZZ_TOOLTIP("踏み切りの瞬間に濃さと枚数をどれだけ増すか。0 で回避のあいだ一定。"
                 "抜け際は Ghost Color そのままの濃さへ戻る")
    // WHY 専用の材質を張るか:
    //   組み込みの残像シェーダーは形の中まで同じ濃さで塗る。もう «そこに無い体» なのに
    //   中身が詰まっていると、半透明なだけの実体が並んでいるように見え、しかも重なった
    //   枚数ぶん濃くなって «団子» になる。PlayerGhost.hlsl は手前を向いた面を抜いて
    //   輪郭だけを残すので、6 枚重ねても濁らない。
    //   色は渡さない ─ 誰の残像か (Ghost Color) とジャスト回避の白熱 (Perfect Color) は
    //   1 回ごとに変わる値で、材質が持てるのは «形» だけ。
    FBZZ_FIELD_FILE(ghostMaterial, "Assets/Materials/Effects/M_PlayerGhost.mat",
                    "Ghost Material", ".mat")
    FBZZ_TOOLTIP("残像の材質 (render_path = \"trail\")。空にすると組み込みの塗り潰しに戻る")

    // ── ジャスト回避 ────────────────────────────────────────────────────────
    // WHY 別の «技» にしないか: ジャスト回避で起きることは «同じ回避が報われた» で
    //     あって、別の動作ではない。同じ残像がその場で明るく長くなる方が、
    //     «今のが良かった» と «何が良かったのか» の両方が 1 つの絵で伝わる。
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
    ///
    /// WHY 新しく出し直さないか: この瞬間に居るのは «かわした姿勢» で、既に
    ///     残像として並んでいる。作り直すと 1 枚目からになるので、いちばん見せたい
    ///     «攻撃を潜り抜けた形» が消える。並んでいるものを塗り替える。
    void Flash();

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;

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
    // 前回 Play / DLL リロードで足した枠は OnDestroy で畳まれている。畳めていない
    // 経路が残っても、Collect が同じ実体を拾い直して上書きするだけで済む。
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

    // WHY 部位を 1 つずつ拾うか: プレイヤーは材質ごとに 10 個の SkinnedMeshRenderer へ
    //     分かれている (P_ArmorWhite / P_GlowGreen …)。MeshTrailComponent は
    //     «その GameObject の描画» を残すので、根に 1 つ付けても何も出ない。
    //
    // WHY «数える» と «足す» を 2 周に分けるか:
    //   AddComponent はシーンの配列を伸ばしうる。1 周で回すと、足した瞬間に
    //   GetChild が返したポインタと self そのものが宙に浮き、次の部位で別の実体を
    //   触りに行く ─ しかも «無効になる» のではなく «別のものとして有効» なので、
    //   症状は «残像が体の一部だけ出ない» のような形で静かに出る。
    const auto note = [&](GameObject& object) {
        if (!object.GetComponent<SkinnedMeshRenderer>()) return;
        m_ghosts.push_back(Ghost{ EntityRef{ object.GetID() }, false });
    };

    const int childCount = self->GetChildCount();
    for (int i = 0; i < childCount; ++i)
        if (GameObject* child = self->GetChild(i)) note(*child);

    // 刀は Player の子ではなくルートに置かれている (WeaponRigComponent の WHY 参照)。
    // 名前で拾うしかないので、抜刀で後から現れる場合に備えて毎回数え直す。
    if (ghostIncludeBlades) {
        const HandSide hands[] = { HandSide::Right };
        for (const HandSide hand : hands)
            if (GameObject* sword = scene.Find(SwordObjectName(hand)))
                note(*sword);
    }

    // 2 周目。ここからは必ず ID から引き直す。
    for (Ghost& ghost : m_ghosts) {
        GameObject* object = ghost.ref.Resolve(scene);
        if (!object) continue;
        if (object->GetComponent<MeshTrailComponent>()) continue;
        ghost.owned = true;
        // シーンへ保存させない。回避の残像は «実行中だけ» の状態で、
        // 保存すると編集画面のプレイヤーが常時ぼやける。
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
    // 出だしが最大で、抜け際に Ghost Color そのままへ戻る。速度の落ち方
    // (Dodge End Speed x) と同じ «線形に落ちる» 形をそのまま借りる。
    const float boost = 1.0f + Max(ghostLead, 0.0f) * (1.0f - Clamp01(progress01));

    Vector4 head = ghostColor;
    head.w = Clamp01(ghostColor.w * boost);
    Paint(head, ghostFade);

    // 濃さだけ上げると «薄い帯が濃くなった» で終わる。刻みも同じ倍率で詰めると
    // 枚数そのものが増え、速い区間だけ帯が «連続した面» になる。
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
        // 材質は毎回書く。Play 中に差し替えても次の回避から効くようにする
        // (パスは .mat 単位で 1 回しか解決しないので、書き続けても重くない)。
        trail->materialPath   = ghostMaterial;
        trail->sampleInterval = Max(ghostInterval, 1.0f / 120.0f);
        trail->minVertexDist  = Max(ghostMinStep, 0.0f);
        trail->maxSamples     = std::clamp(ghostMaxSamples, 1, 16);
        // 裏面も描く。転がりは体が丸まるので、片面だけだと «内側» が抜けて
        // 残像が空洞に見える。
        trail->doubleSided    = true;
        // WHY 自然消滅させるか: 回避が終わった «瞬間» に残像が全部消えると、
        //     抜け切ったことより先に «絵が急に無くなった» と読まれる。記録だけ止めて、
        //     並んでいる枚数は寿命で薄れさせる。
        trail->clearOnDisable = false;
        // 前の回避の枚数を引き継ぐと、跳んだ先と前回の位置が 1 本に繋がって見える。
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
            trail->enabled = false;   // clearOnDisable = false なので寿命で薄れる
    m_active = false;
}

inline void DodgeAfterimageComponent::Flash()
{
    // 回避の «外» から呼ばれる。残像が 1 枚も無いフレーム (回避していない被弾) では
    // 塗る相手が居ないので、そのまま何も起きない。
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
            // 足したのはこちら。samples の GPU 資源は «消す» 経路を通してから外す。
            trail->enabled        = false;
            trail->clearOnDisable = true;
            trail->clearRequested = true;
        } else {
            // シーンが持っていた個体。作者の設定を壊さないよう、切るだけにする。
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

    // WHY 番号で «出た» を取るか: IsDodging の立ち上がりだけを見ると、回避が
    //     途切れずに連続したフレーム (硬直明けの入力が預かりから出た場合) で
    //     1 回目と 2 回目の残像が 1 本に繋がる。番号は 1 回ごとに必ず進む。
    if (dodging && serial != m_lastSerial) {
        m_lastSerial = serial;
        Begin();
        return;
    }

    // 回避のあいだは速度カーブに合わせて濃さと刻みを書き直し続ける。
    // ジャスト回避で塗り替えた後は触らない ─ そこは «速さ» ではなく «報われた» の絵で、
    // 減速に合わせて薄め直すと、良かったと言った次のフレームに自分で取り消すことになる。
    if (dodging && m_active && !m_flux) Drive(m_controller->DodgeProgress01());

    if (!dodging && m_active) Stop();
}

} // namespace sandbox
