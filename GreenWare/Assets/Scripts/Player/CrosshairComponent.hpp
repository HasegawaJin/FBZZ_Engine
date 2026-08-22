/// @file CrosshairComponent.hpp
/// @brief 画面中央のクロスヘア。線を引く 1 点を画面に固定して見せる
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 画面中央に固定するか:
///   6.4 でロックオンを廃したので、線の向きは視線そのものになった。
///   PlayerAimComponent はカメラの前方へレイを張るため、その到達点は必ず画面中央に写る。
///   そこへクロスヘアを置くと、照射の判定・レーザーの絵・上半身のエイム姿勢・首の向きが
///   すべて同じ 1 ピクセルを指していることが、プレイヤーから直接見える。
///
///   カメラの追従を緩めて走行中にプレイヤーが画面内で流れても (TpsCameraComponent の
///   Follow Slack)、狙う点は動かない。「自分は流れるが照準は固定」という関係は、
///   中央に基準が出ていて初めて読める。
///
/// WHY スプライトと手続き描画の両方を持つか:
///   絵が用意できていない段階でも照準が出ていないと、6.2 のなぞりを一度も試せない。
///   手続きの 4 本 ＋ 中心点を既定にしておき、Crosshair Sprite を割り当てたら
///   そちらへ切り替わる。差し替えのために配置や色の流し込みを書き直さなくて済む。
///
/// WHY 手続き版が 4 本 ＋ 中心点か:
///   中心点だけだと 6.2 のビーム半径 0.6m がどれくらいの広がりなのか読めない。
///   4 本だけだと、遠くの敵に重ねたときに中心が抜けて狙点が消える。
///   間隔 (Gap) がそのままビームの太さの目安になり、中心点が正確な 1 点を保証する。
///
/// WHY ランタイムで組むか:
///   Canvas ＋ 4 本 ＋ 中心点 ＋ スプライト = 7 GameObject。シーンへ保存すると
///   階層が 7 行増えるうえ、太さを変える作業が 10 箇所の数値を入れ直すことになる。
///   runtimeGenerated で組めば、シーンに残るのはこのスクリプト 1 行だけになる。
#pragma once

#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PolarityGunComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class CrosshairComponent : public Script {
    FBZZ_SCRIPT(CrosshairComponent)

public:
    FBZZ_GROUP("Sprite")
    // WHY 手続きの 4 本を残したまま差し替え式にするか:
    //   絵が用意できていない段階でも照準が出ていないと、6.2 のなぞりを一切試せない。
    //   スプライトを入れたらそちらが正になり、手続きの 4 本は自動的に引っ込む。
    //
    // WHY ファイルパスではなく SpriteRef か: GUID を正本に持つので、アトラスを
    //   別フォルダへ移しても参照が切れない。Inspector でもスプライトだけが
    //   ドロップ対象になり、テクスチャ全体を渡してしまう事故が起きない。
    FBZZ_ASSET_FIELD(SpriteRef, crosshairSprite, "Crosshair Sprite")
    FBZZ_TOOLTIP("割り当てると 4 本 + 中心点の代わりにこの 1 枚を出す。空で手続き描画")
    FBZZ_FIELD_RANGE(float, spriteSize, 48.0f, "Sprite Size", 4.0f, 512.0f)
    FBZZ_TOOLTIP("一辺のキャンバスピクセル。1920x1080 基準")

    FBZZ_GROUP("Layout")
    // 単位は 1920x1080 を基準にしたキャンバスピクセル。手続き描画のときだけ効く。
    FBZZ_FIELD_RANGE(float, gap, 9.0f, "Gap", 0.0f, 80.0f)
    FBZZ_TOOLTIP("中心の空き。ここが狙点そのものなので、広げるほど 1 点が曖昧になる")
    FBZZ_FIELD_RANGE(float, tickLength, 13.0f, "Tick Length", 1.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, tickThickness, 2.5f, "Tick Thickness", 1.0f, 12.0f)
    FBZZ_FIELD_RANGE(float, dotSize, 3.0f, "Dot Size", 0.0f, 20.0f)
    FBZZ_TOOLTIP("中心点の一辺。0 で出さない")

    FBZZ_GROUP("Feedback")
    // WHY 照射中に開くか: 押した瞬間に絵が変わらないと、6.3 のバッテリーを
    //     消費し始めたことが画面のどこにも出ない。HUD のゲージは視界の端にあり、
    //     線を引いている最中は見ていない。
    FBZZ_FIELD_RANGE(float, emitSpread, 7.0f, "Emit Spread", 0.0f, 60.0f)
    FBZZ_TOOLTIP("照射中に 4 本が外へ開く量。0 で開かない")
    FBZZ_FIELD_RANGE(float, spreadResponse, 16.0f, "Spread Response", 1.0f, 60.0f)

    FBZZ_GROUP("Color")
    // 12.2 の配色に従い、既定はプレイヤーの緑。極の赤青とは競合しない。
    FBZZ_FIELD_COLOR(idleColor, (Vector4{ 0.20f, 1.00f, 0.45f, 0.85f }), "Idle Color")
    FBZZ_FIELD(bool, showPolarity, true, "Show Polarity")
    FBZZ_TOOLTIP("照射中はその極の色、非照射中は線の先頭に居る敵が帯びている極の色にする。"
                 "切ると常に Idle Color")

    FBZZ_GROUP("Holstered")
    // 銃を収めている間に照準だけ残ると、撃てるのに撃たない状態に見える。
    FBZZ_FIELD_RANGE(float, holsteredAlpha, 0.0f, "Holstered Alpha", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, fadeSpeed, 9.0f, "Fade Speed", 1.0f, 40.0f)

    /// PlayerComponent が内部モジュールとして持つときに、同じ参照を渡す。
    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    void SetGun(PolarityGunComponent* gun) { m_gunOverride = gun; }
    void SetWeaponRig(WeaponRigComponent* rig) { m_weaponRig = rig; }

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    /// 上下左右の 4 本。中心点とスプライトはこれとは別に 1 枚ずつ持つ。
    static constexpr int kTickCount = 4;
    /// Canvas の子の総数。4 本 + 中心点 + スプライト。
    static constexpr int kPartCount = kTickCount + 2;
    static constexpr const char* kCanvasName = "Crosshair";
    // HUD_Canvas は 10。照準は常にその手前に出す。
    static constexpr int kCanvasSortOrder = 20;
    // Canvas Scaler の基準解像度。HUD と同じ土俵で寸法を決められるようにする。
    static constexpr float kReferenceWidth  = 1920.0f;
    static constexpr float kReferenceHeight = 1080.0f;

    [[nodiscard]] PlayerAimComponent*   Aim() const;
    [[nodiscard]] PolarityGunComponent* Gun() const;
    [[nodiscard]] std::string CanvasName() const;
    /// 今フレームの色。照射中の極 → 線の先頭の極 → Idle の順に決まる。
    [[nodiscard]] Vector4 CurrentColor() const;

    /// スプライトが割り当たっているか。手続き描画との切り替えはこれ 1 つで決まる。
    [[nodiscard]] bool UsesSprite() const { return crosshairSprite.IsValid(); }

    /// 生成済みの照準を拾い直せたら true。スクリプト DLL のリロード対策。
    bool Adopt();
    void Build();
    /// 中心からの空き spread で 4 本と中心点を並べる。
    void Layout(float spread);
    /// スプライト 1 枚を中心へ置く。spread のぶんだけ広げる。
    void LayoutSprite(float spread);

    PlayerAimComponent*   m_aimOverride = nullptr;
    PolarityGunComponent* m_gunOverride = nullptr;
    WeaponRigComponent*   m_weaponRig   = nullptr;

    EntityRef m_canvas;
    std::array<EntityRef, kTickCount> m_ticks{};
    EntityRef m_dot;
    EntityRef m_sprite;
    // 最後に流したスプライトのパス。毎フレーム SetImageTexture を呼ぶと
    // そのたびにテクスチャの解決が走るため、変わったときだけ送る。
    std::string m_appliedSprite;

    // 開き具合と表示の濃さ。どちらも明滅を避けるため補間する。
    float m_spread   = 0.0f;
    float m_presence = 0.0f;
};

FBZZ_REFLECT(CrosshairComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline PlayerAimComponent* CrosshairComponent::Aim() const
{
    return m_aimOverride ? m_aimOverride : scene.GetScript<PlayerAimComponent>();
}

inline PolarityGunComponent* CrosshairComponent::Gun() const
{
    return m_gunOverride ? m_gunOverride : scene.GetScript<PolarityGunComponent>();
}

inline std::string CrosshairComponent::CanvasName() const
{
    // WHY 持ち主ごとに名前を変えるか: 照準はルートに置くため、名前で拾い直すときに
    //     同名だと 2 人目のプレイヤー (デバッグ用の複製を含む) が 1 人目の照準を奪う。
    GameObject* owner = scene.Self();
    return std::string(kCanvasName) + "_" + (owner ? owner->instanceId : std::string{});
}

inline void CrosshairComponent::OnStart()
{
    m_spread   = 0.0f;
    m_presence = 0.0f;
    m_appliedSprite.clear();

    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     下の EntityRef は空に戻る。一方 UI の GameObject は Scene 側に残っているので、
    //     無条件に組み直すとリロードのたびに照準が 1 組ずつ増えていく。
    if (!Adopt()) Build();
}

inline bool CrosshairComponent::Adopt()
{
    GameObject* canvasObject = scene.Find(CanvasName());
    if (!canvasObject || !canvasObject->GetComponent<UICanvas>()) return false;
    if (canvasObject->GetChildCount() < kPartCount) return false;

    for (int i = 0; i < kTickCount; ++i) {
        GameObject* child = canvasObject->GetChild(i);
        if (!child) return false;
        m_ticks[static_cast<std::size_t>(i)] = EntityRef{ child->GetID() };
    }
    GameObject* dot    = canvasObject->GetChild(kTickCount);
    GameObject* sprite = canvasObject->GetChild(kTickCount + 1);
    if (!dot || !sprite) return false;

    m_dot    = EntityRef{ dot->GetID() };
    m_sprite = EntityRef{ sprite->GetID() };
    m_canvas = EntityRef{ canvasObject->GetID() };
    m_appliedSprite.clear();
    return true;
}

inline void CrosshairComponent::Build()
{
    GameObject& canvasObject = scene.Create(CanvasName());
    canvasObject.runtimeGenerated = true;

    UICanvas& canvas = canvasObject.AddComponent<UICanvas>();
    canvas.renderMode = UIRenderMode::ScreenSpaceOverlay;
    canvas.sortOrder  = kCanvasSortOrder;
    // WHY HUD_Canvas と違って Scale With Screen Size か: Constant Pixel Size は
    //     1920x1080 の矩形をそのまま画面全体へ引き伸ばす。16:9 以外の解像度では
    //     縦横で倍率が変わり、正方形のはずの中心点が長方形になる。
    //     照準は「中心が 1 点である」ことが仕事なので、歪みを許容できない。
    canvas.scaleMode          = UICanvasScaleMode::ScaleWithScreenSize;
    canvas.referenceWidth     = kReferenceWidth;
    canvas.referenceHeight    = kReferenceHeight;
    // 幅基準と高さ基準の幾何平均。どちらの辺が伸びても照準の大きさが暴れない。
    canvas.matchWidthOrHeight = 0.5f;

    const auto addPart = [&](int sortOrder) {
        GameObject& part = scene.Create(kCanvasName + std::string("_Part"));
        part.runtimeGenerated = true;
        part.SetParent(canvasObject);
        UIImage& image = part.AddComponent<UIImage>();
        // 画面中央を基準に、自分の中心をそこへ合わせる。以降 transform.position は
        // 「中心からのずれ」だけを意味するようになり、解像度が変わっても崩れない。
        image.anchoring.anchor = { 0.5f, 0.5f };
        image.anchoring.pivot  = { 0.5f, 0.5f };
        image.sortOrder = sortOrder;
        return EntityRef{ part.GetID() };
    };

    for (int i = 0; i < kTickCount; ++i)
        m_ticks[static_cast<std::size_t>(i)] = addPart(0);
    m_dot = addPart(1);
    // スプライトは手続きの 4 本より手前。差し替えたときに古い形が透けない。
    m_sprite = addPart(2);

    m_canvas = EntityRef{ canvasObject.GetID() };
    m_appliedSprite.clear();
}

inline void CrosshairComponent::Layout(float spread)
{
    const float thickness = Max(tickThickness, 1.0f);
    const float length    = Max(tickLength, 1.0f);
    // 4 本の中心までの距離。空き + 棒の半分。
    const float offset    = Max(gap, 0.0f) + spread + length * 0.5f;

    // 0=上 / 1=下 / 2=左 / 3=右。position は中心からのずれ、scale.xy が幅と高さ。
    // Canvas の y は下向きなので、上の棒は負のずれになる。
    const Vector3 placement[kTickCount] = {
        {  0.0f, -offset, 0.0f },
        {  0.0f,  offset, 0.0f },
        { -offset, 0.0f,  0.0f },
        {  offset, 0.0f,  0.0f },
    };
    const Vector3 extent[kTickCount] = {
        { thickness, length, 1.0f },
        { thickness, length, 1.0f },
        { length, thickness, 1.0f },
        { length, thickness, 1.0f },
    };

    for (int i = 0; i < kTickCount; ++i) {
        GameObject* tick = m_ticks[static_cast<std::size_t>(i)].Resolve(scene);
        if (!tick) continue;
        tick->transform.position = placement[i];
        tick->transform.scale    = extent[i];
    }

    if (GameObject* dot = m_dot.Resolve(scene)) {
        const float size = Max(dotSize, 0.0f);
        dot->transform.position = Vector3::ZERO;
        dot->transform.scale    = { size, size, 1.0f };
    }
}

inline void CrosshairComponent::LayoutSprite(float spread)
{
    GameObject* sprite = m_sprite.Resolve(scene);
    if (!sprite) return;

    // WHY 毎フレーム送らないか: ResolvePath() は GUID からパスを引き直し、
    //     SetImageTexture はそのたびにテクスチャを解決する。変わったときだけでよい。
    if (std::string path = crosshairSprite.ResolvePath(); m_appliedSprite != path) {
        ui.SetImageTexture(sprite, path);
        m_appliedSprite = std::move(path);
    }

    // 開きは 4 本のときと同じ意味 (中心から外へ広がる) にしたいので、
    // 直径へ両側ぶんを足す。倍率で拡大すると線の太さまで太る。
    const float size = Max(spriteSize, 1.0f) + spread * 2.0f;
    sprite->transform.position = Vector3::ZERO;
    sprite->transform.scale    = { size, size, 1.0f };
}

inline Vector4 CrosshairComponent::CurrentColor() const
{
    if (!showPolarity) return idleColor;

    // 照射中はその極の色。「今どちらを塗っているか」は、押しているボタンより
    // 画面の色で確認するほうが速い。
    if (auto* gun = Gun()) {
        const bool plus  = gun->IsEmitting(Polarity::Plus);
        const bool minus = gun->IsEmitting(Polarity::Minus);
        // 両方同時なら、どちらか一方の色に倒すと嘘になる。中間の白へ寄せる。
        if (plus && minus) return { 1.0f, 1.0f, 1.0f, idleColor.w };
        if (plus)  return { kColorPlus.x,  kColorPlus.y,  kColorPlus.z,  idleColor.w };
        if (minus) return { kColorMinus.x, kColorMinus.y, kColorMinus.z, idleColor.w };
    }

    // 照射していない間は、線の先頭に居る敵が今どちらを帯びているかを出す。
    // 7.9 の起爆点は「無極のまま温存した 1 体」なので、無極は Idle 色のままが正しい。
    if (auto* aim = Aim()) {
        if (const auto* target = aim->CurrentPolarityTarget();
            target && target->Current() != Polarity::None) {
            const Vector4 color = PolarityColor(target->Current());
            return { color.x, color.y, color.z, idleColor.w };
        }
    }
    return idleColor;
}

inline void CrosshairComponent::OnLateUpdate()
{
    GameObject* canvasObject = m_canvas.Resolve(scene);
    if (!canvasObject) return;

    auto* gun = Gun();
    const bool emitting = gun && (gun->IsEmitting(Polarity::Plus)
                               || gun->IsEmitting(Polarity::Minus));
    const bool drawn = !m_weaponRig || m_weaponRig->IsDrawn();

    // 開きも濃さも実時間で進める。ヒットストップ中に固まると、止めが解けた瞬間に
    // 照準だけが跳ねて、当たった手応えより先に目に入る。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    const float targetSpread = emitting ? Max(emitSpread, 0.0f) : 0.0f;
    m_spread += (targetSpread - m_spread) * (1.0f - std::exp(-Max(spreadResponse, 0.0f) * dt));

    // どちらの描き方を使うかは 1 箇所で決め、使わない側は要素ごと止める。
    // 位置だけずらして隠すと、次にスプライトを外したときに古い並びが残る。
    const bool sprite = UsesSprite();
    for (const EntityRef& tick : m_ticks)
        ui.SetImageEnabled(tick.Resolve(scene), !sprite);
    ui.SetImageEnabled(m_dot.Resolve(scene), !sprite);
    ui.SetImageEnabled(m_sprite.Resolve(scene), sprite);

    if (sprite) LayoutSprite(m_spread);
    else        Layout(m_spread);

    const float targetPresence = drawn ? 1.0f : Clamp01(holsteredAlpha);
    m_presence += (targetPresence - m_presence) * (1.0f - std::exp(-Max(fadeSpeed, 0.0f) * dt));

    const Vector4 color    = CurrentColor();
    const float   presence = Clamp01(m_presence);
    const Vector4 tinted{ color.x, color.y, color.z, color.w * presence };

    if (sprite) {
        // 色は乗算で乗る。白いスプライトを入れると 12.2 の極性色がそのまま出て、
        // 色の付いた絵を入れるとその色を保ったまま濃さだけが動く。
        ui.SetImageColor(m_sprite.Resolve(scene), tinted);
        return;
    }

    for (const EntityRef& tick : m_ticks)
        ui.SetImageColor(tick.Resolve(scene), tinted);
    ui.SetImageColor(m_dot.Resolve(scene), tinted);
}

inline void CrosshairComponent::OnDestroy()
{
    // ルートに置いた以上、プレイヤーと一緒には消えない。持ち主が畳む。
    if (GameObject* canvasObject = m_canvas.Resolve(scene))
        scene.Destroy(*canvasObject);
    m_canvas = {};
    m_ticks.fill(EntityRef{});
    m_dot    = {};
    m_sprite = {};
    m_appliedSprite.clear();
}

} // namespace sandbox
