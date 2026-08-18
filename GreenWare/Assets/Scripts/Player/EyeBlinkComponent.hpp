// FBZZ Engine
// EyeBlinkComponent.hpp | sandbox
// 目のスプライトを差し替えて瞬きさせる。
//
// 仕組み:
//   開き目 / 閉じ目の 2 枚のスプライトを Inspector で受け取り、対象 Material
//   の albedo (t0) を GameObject 単位で上書きして切り替える。
//   材質は Materials/Skinned/SkinnedEyeSprite.mat (専用シェーダー) を想定する。
//
// WHY ボーンやモーフではなくテクスチャ差し替えか:
//   瞬きのために目のまわりへ専用ボーンを入れると、アニメーションクリップ 4 本すべてに
//   まぶたのキーを打つ必要があり、瞬きの間隔をゲーム側で決められなくなる。
//   絵の差し替えなら 1 スクリプトで完結し、間隔もアニメーションと独立に持てる。
//
// WHY 2 枚を混ぜず即時カットで切り替えるか:
//   瞬きは 2〜3 フレームで閉じ切る速い動きで、中間状態はほぼ見えない。半透明で
//   混ぜると開いた目と閉じた目が重なって目が二重に見える。混ぜない代わりに、
//   閉じている時間 (closedDuration) を短く保つことで速さを表現する。
//
// WHY 間隔をランダムにするか:
//   等間隔で瞬くと機械の点滅に見え、生き物として読めない。人間の自然な瞬きは
//   3〜6 秒に 1 回で間隔がばらつき、ときどき 2 回続く。その 2 つを再現する。
#pragma once

#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Script.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EyeBlinkComponent : public Script {
    FBZZ_SCRIPT(EyeBlinkComponent)

    // 差し替え先の Material が要る。
    // WHY REQUIRE ではなく OPTIONAL か: eyeRenderer で別 GameObject を指した場合、
    //     Material は自分ではなくそちらに付く。自分に必須と宣言すると、正しい構成が
    //     Inspector 上で「不足」と警告され続けることになる。
    //     解決できなかった場合は OnStart が実行時エラーとして報告する。
    FBZZ_OPTIONAL_COMPONENT(MaterialComponent)

public:
    FBZZ_GROUP("Sprites")
    FBZZ_ASSET_FIELD(SpriteRef, openSprite, "Open Eyes")
    FBZZ_TOOLTIP("通常時の目。アトラス内の 1 コマを指定してよい")
    FBZZ_ASSET_FIELD(SpriteRef, closedSprite, "Closed Eyes")
    FBZZ_TOOLTIP("瞬きの瞬間だけ表示する閉じ目")

    FBZZ_GROUP("Target Material")
    // 目のマテリアルを持つ GameObject。未アサインなら自分自身。
    // WHY 参照で持たせるか: SkinnedMeshRenderer は 1 GameObject でモデル全体を描くため、
    //     目のマテリアルは通常この Script と同じ GameObject のスロットに並ぶ。ただし
    //     プレイヤーのルートに制御スクリプトを集める構成では、描画は子の GameObject に
    //     なる。どちらの構成でも動くよう、既定は自分・必要なら指定に開けておく。
    FBZZ_REF(GameObject, eyeRenderer, "Eye Renderer")
    FBZZ_TOOLTIP("目のマテリアルを持つ GameObject。空なら自分自身。先頭の Material を使用")

    FBZZ_GROUP("Timing")
    FBZZ_FIELD_RANGE(float, intervalMin, 2.8f, "Interval Min", 0.2f, 15.0f)
    FBZZ_TOOLTIP("瞬きから次の瞬きまでの最短秒数")
    FBZZ_FIELD_RANGE(float, intervalMax, 6.0f, "Interval Max", 0.2f, 15.0f)
    FBZZ_TOOLTIP("同・最長秒数。Min より小さくても自動で入れ替える")
    FBZZ_FIELD_RANGE(float, closedDuration, 0.09f, "Closed Duration", 0.02f, 0.6f)
    FBZZ_TOOLTIP("目を閉じている秒数。0.1 秒前後より長いと眠そうに見える")
    FBZZ_FIELD_RANGE_INT(int, doubleBlinkPercent, 25, "Double Blink %", 0, 100)
    FBZZ_TOOLTIP("2 回続けて瞬く確率")
    FBZZ_FIELD_RANGE(float, doubleBlinkGap, 0.11f, "Double Blink Gap", 0.02f, 0.5f)
    FBZZ_TOOLTIP("2 連の 1 回目と 2 回目の間で目を開けている秒数")

    FBZZ_GROUP("Debug")
    // WHY bool ではなく文字列か: IReflector::Readonly に bool 版が無く、
    //     暗黙変換で int 版に落ちて 0 / 1 と表示される。開閉は一目で読めるべき値なので、
    //     PolarityBodyComponent の debugPhase と同じく状態名をそのまま出す。
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Open", "State")
    FBZZ_FIELD_READ_ONLY(float,       debugTimer, 0.0f,   "Phase Remaining")

    // ── 外から呼ぶ入口 ───────────────────────────────────────────────────────
    [[nodiscard]] bool IsClosed() const { return m_closed; }

    // 今すぐ 1 回瞬く。被弾・驚きなどの反応に使う。
    // 瞬きの途中で呼ばれた場合は閉じ時間をやり直すだけにする (二重に閉じない)。
    void Blink();

    // 瞬きループを止める / 再開する。死亡演出やカットシーンで、目の状態を
    // 別のスクリプトが握りたいときに使う。
    // WHY 目を開いた状態へ戻さないか: 「止めた瞬間の絵をそのまま保つ」が最も
    //     使いやすい。死亡時は ForceEyes(true) → SetSuppressed(true) の順で
    //     呼べば閉じたまま固定でき、開かせたい側は ForceEyes(false) を選べる。
    void SetSuppressed(bool suppressed);

    // 状態を直接指定する。SetSuppressed(true) と組み合わせて固定表情を作る。
    void ForceEyes(bool closed);

    void OnStart()  override;
    void OnUpdate() override;

private:
    // 差し替えを実際に material へ書く。状態が変わったときだけ呼ぶ。
    void ApplySprite(bool closed);
    // 次の瞬きまでの秒数を引き、同時に「今回は何連か」を決める。
    [[nodiscard]] float RollNextInterval();
    [[nodiscard]] MaterialInstance EyeMaterial() const;

    // Sprite 参照をテクスチャ差し替え用の型へ移し替えたもの。
    // WHY OnStart で作って持つか: ScriptAssetReference の解決は GUID → パスの
    //     逆引きを伴う。瞬きは 3 秒ごととはいえ、毎回引き直す理由がない。
    TextureRef m_openTexture;
    TextureRef m_closedTexture;

    // 現在のフェーズの残り時間。開いている間は次の瞬きまで、閉じている間は
    // 開くまで、2 連の合間は 2 回目までを表す。
    float m_timer  = 0.0f;
    bool  m_closed = false;
    // この瞬きであと何回閉じるか。2 なら 2 連。開いた瞬間に 1 減らす。
    int   m_remainingBlinks = 1;
    bool  m_suppressed = false;
    // 材質へ 1 度も書けていないか。OnStart で開き目を必ず 1 回書き込むためのフラグ。
    bool  m_applied = false;
};

FBZZ_REFLECT(EyeBlinkComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

// 目のスプライトを差し込む Material のテクスチャスロット。
//
// WHY "albedo" ではなく "t0" と書くか:
//   MaterialInstance::SetTexture はシェーダーリフレクションで名前を検証する。
//   リフレクションが返すのは HLSL の変数名 (texAlbedo) なので "albedo" では
//   一致せず、書き込みが警告だけ出して静かに捨てられる。"t0" はレジスタ番号として
//   解釈され、スロット一致で検証を通ったうえで正規名 "albedo" のキーへ格納される。
inline constexpr MaterialPropertyId kAlbedoTextureId{ "t0" };

inline MaterialInstance EyeBlinkComponent::EyeMaterial() const
{
    // eyeRenderer 未アサインなら EntityRef が無効になり、Instance が自分自身へ落ちる。
    // MaterialInstance は指定先 GameObject の先頭 Material を使用する。
    // WHY index を持たないか: 瞳専用 Renderer は専用 Material を 1 つだけ持つ構成に統一し、
    //      モデルの submesh 配列変更で Inspector の番号がずれる問題をなくす。
    return material.Instance(eyeRenderer.ref);
}

inline void EyeBlinkComponent::OnStart()
{
    // Sprite 参照 (guid + アトラス内のコマ) をそのまま引き継ぐ。
    // WHY ResolvePath() を経由しないか: 文字列へ落とすと GUID が失われ、
    //     画像を移動したあとに参照が切れる。参照構造ごと移せば両方残る。
    m_openTexture.reference   = openSprite.reference;
    m_closedTexture.reference = closedSprite.reference;

    if (!openSprite.IsValid() || !closedSprite.IsValid()) {
        // 片方でも欠けると「瞬いているのに絵が変わらない」か「目が消える」になり、
        // どちらも原因がスクリプト側からは見えない。ここで必ず声を上げる。
        debug.LogError("EyeBlinkComponent requires both Open Eyes and Closed Eyes "
                       "sprites (blinking does nothing without them).");
    }
    if (!EyeMaterial().IsValid()) {
        debug.LogError("EyeBlinkComponent found no Material to write to "
                       "(check Eye Renderer and its first Material).");
    }

    m_suppressed = false;
    m_closed     = false;
    m_applied    = false;
    ApplySprite(false);
    m_timer = RollNextInterval();
}

inline float EyeBlinkComponent::RollNextInterval()
{
    // 2 連にするかを「次の瞬き」の抽選と同時に決める。閉じる直前に決めると、
    // Blink() で割り込んだ回まで 2 連の抽選対象になり、反応の瞬きが勝手に 2 回出る。
    m_remainingBlinks = random.Chance(static_cast<float>(doubleBlinkPercent) * 0.01f) ? 2 : 1;

    // Inspector で Min > Max に設定されても止まらないよう、ここで並べ直す。
    const float low  = Min(intervalMin, intervalMax);
    const float high = Max(intervalMin, intervalMax);
    return random.Range(low, high);
}

inline void EyeBlinkComponent::ApplySprite(bool closed)
{
    // 論理状態は材質が引けたかに関わらず必ず進める。
    // WHY: 書き込み失敗で m_closed を据え置くと、状態機械が「まだ開いている」と
    //      判断して closedDuration ごとに閉じ直そうとし続け、瞬きの周期そのものが
    //      壊れる。絵が出ないことと時間の進行は分けて扱う。
    const bool changed = !m_applied || m_closed != closed;
    m_closed   = closed;
    debugState = closed ? "Closed" : "Open";

    // 状態が変わっていないなら書かない。SetTexture は検証とマップ書き込みを伴い、
    // 毎フレーム呼ぶと 3 秒に 1 回で足りる処理を 180 倍走らせることになる。
    if (!changed) return;

    const MaterialInstance instance = EyeMaterial();
    // 解決できなかったときは m_applied を立てない。次に状態が変わったとき再試行され、
    // 材質が後から揃った場合に自動で復帰する。
    if (!instance.IsValid()) return;

    instance.SetTexture(kAlbedoTextureId, closed ? m_closedTexture : m_openTexture);
    m_applied = true;
}

inline void EyeBlinkComponent::Blink()
{
    if (m_suppressed) return;

    // すでに閉じているなら閉じ時間をやり直すだけ。ここで再度閉じる扱いにすると
    // m_remainingBlinks が減らないまま閉じ直し、連打で目が開かなくなる。
    ApplySprite(true);
    m_timer = closedDuration;
}

inline void EyeBlinkComponent::SetSuppressed(bool suppressed)
{
    if (m_suppressed == suppressed) return;
    m_suppressed = suppressed;
    // 再開時は必ず開いた状態から数え直す。止めている間に閉じたまま固定されていた
    // 場合、そのまま次の「開く」を待つと閉じ目のまま数秒放置される。
    if (!suppressed) {
        ApplySprite(false);
        m_timer = RollNextInterval();
    }
}

inline void EyeBlinkComponent::ForceEyes(bool closed)
{
    ApplySprite(closed);
    // 抑制中でなければ、指定した状態を最低 1 フェーズぶんは保つ。
    if (!m_suppressed)
        m_timer = closed ? closedDuration : RollNextInterval();
}

inline void EyeBlinkComponent::OnUpdate()
{
    if (m_suppressed) {
        debugTimer = 0.0f;
        return;
    }

    m_timer -= Time::deltaTime;
    debugTimer = Max(0.0f, m_timer);
    if (m_timer > 0.0f) return;

    if (m_closed) {
        // 閉じ終わり。開けてから、2 連の残りがあるかで次の待ち時間を決める。
        ApplySprite(false);
        m_remainingBlinks -= 1;
        m_timer = (m_remainingBlinks > 0) ? doubleBlinkGap : RollNextInterval();
        return;
    }

    // 開いている時間が終わった → 閉じる。
    ApplySprite(true);
    m_timer = closedDuration;
}

} // namespace sandbox
