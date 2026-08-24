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
/// WHY 形を UIImage の並びではなくシェーダーに持たせるか:
///   4 本 + 中心点を矩形で並べていたときは、太さを 1px 変えるだけで 10 箇所の数値を
///   入れ直すことになり、「中心が 1 点である」ことは 4 つの座標がたまたま揃っている
///   状態でしか成り立たなかった。しかも矩形は端が硬いまま縁も付けられない。
///   形を UICrosshair.hlsl (距離場) へ移すと、要素は Canvas + 1 枚になり、
///   このスクリプトの仕事は「今の色と開きを送る」だけになる。
///
/// WHY 手続きの形が 4 本 ＋ 中心点か:
///   中心点だけだと 6.2 のビーム半径 0.6m がどれくらいの広がりなのか読めない。
///   4 本だけだと、遠くの敵に重ねたときに中心が抜けて狙点が消える。
///   間隔 (Gap) がそのままビームの太さの目安になり、中心点が正確な 1 点を保証する。
///
/// WHY スプライトも同じ 1 枚で出すか:
///   絵が用意できていない段階でも照準が出ていないと、6.2 のなぞりを一度も試せない。
///   図形とスプライトを別の要素に分けると、差し替えのたびに片方を止める処理が要り、
///   止め忘れれば 2 つの照準が重なる。マテリアルの spriteMode で切り替えれば、
///   どちらで出していても色と濃さの流し込みは 1 本のままになる。
///
/// WHY ランタイムで組むか:
///   Canvas + 1 枚とはいえ、シーンへ保存すると照準の寸法がシーン側とこのスクリプト側の
///   2 箇所に散る。runtimeGenerated で組めば、シーンに残るのはこのスクリプト 1 行だけになる。
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
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class CrosshairComponent : public Script {
    FBZZ_SCRIPT(CrosshairComponent)

public:
    FBZZ_GROUP("Material")
    // WHY 既定のパスを持たせるか: 形はシェーダーが描くので、割り当てが無いと
    //   照準は「白い四角」にしかならない。空欄のときだけ差し替えたい人が触ればよい。
    FBZZ_FIELD_FILE(crosshairMaterial, "Assets/Materials/UI/UICrosshair.mat",
                    "Crosshair Material", ".mat")
    FBZZ_TOOLTIP("照準を描く UI マテリアル。UICrosshair.hlsl を割り当てたものを指す")

    FBZZ_GROUP("Sprite")
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
    FBZZ_FIELD_RANGE(float, tickRoundness, 1.0f, "Tick Roundness", 0.0f, 1.0f)
    FBZZ_TOOLTIP("線の端の丸み。0 で切り落とし、1 で半円")
    FBZZ_FIELD_RANGE(float, dotSize, 3.0f, "Dot Size", 0.0f, 20.0f)
    FBZZ_TOOLTIP("中心点の直径。0 で出さない")

    FBZZ_GROUP("Outline")
    // WHY 縁取りを持つか: 12.2 の照準色はプレイヤーの緑で、この作品の背景は草と木で
    //     埋まる。同系色の上に細い緑線を置くと輪郭が溶けて狙点が消える。
    FBZZ_FIELD_COLOR(outlineColor, (Vector4{ 0.01f, 0.03f, 0.02f, 0.85f }), "Outline Color")
    FBZZ_FIELD_RANGE(float, outlineWidth, 1.0f, "Outline Width", 0.0f, 8.0f)
    FBZZ_TOOLTIP("線と中心点を囲む縁の太さ。0 で縁を出さない")

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
    static constexpr const char* kCanvasName = "Crosshair";
    // HUD_Canvas は 10。照準は常にその手前に出す。
    static constexpr int kCanvasSortOrder = 20;
    // Canvas Scaler の基準解像度。HUD と同じ土俵で寸法を決められるようにする。
    static constexpr float kReferenceWidth  = 1920.0f;
    static constexpr float kReferenceHeight = 1080.0f;
    // 矩形の縁とアンチエイリアスのための余白。ここが 0 だと開ききった線の先が切れる。
    static constexpr float kEdgeMargin = 2.0f;

    [[nodiscard]] PlayerAimComponent*   Aim() const;
    [[nodiscard]] PolarityGunComponent* Gun() const;
    [[nodiscard]] std::string CanvasName() const;
    /// 今フレームの色。照射中の極 → 線の先頭の極 → Idle の順に決まる。
    [[nodiscard]] Vector4 CurrentColor() const;
    /// 照準を収める矩形の一辺 (Canvas ピクセル)。
    [[nodiscard]] float ElementSize() const;

    /// スプライトが割り当たっているか。どちらの描き方を使うかはこれ 1 つで決まる。
    [[nodiscard]] bool UsesSprite() const { return crosshairSprite.IsValid(); }

    /// 生成済みの照準を拾い直せたら true。スクリプト DLL のリロード対策。
    bool Adopt();
    void Build();
    /// 形と色をマテリアルへ流す。ここに送った値はこの要素の描画にだけ乗る。
    void PushMaterial(GameObject* element, const Vector4& color, bool sprite) const;

    PlayerAimComponent*   m_aimOverride = nullptr;
    PolarityGunComponent* m_gunOverride = nullptr;
    WeaponRigComponent*   m_weaponRig   = nullptr;

    EntityRef m_canvas;
    EntityRef m_element;
    // 最後に流したスプライトのパス。毎フレーム SetImageTexture を呼ぶと
    // そのたびにテクスチャの解決が走るため、変わったときだけ送る。
    std::string m_appliedSprite;
    bool m_reportedMissingMaterial = false;

    // 開き具合と表示の濃さ。どちらも明滅を避けるため補間する。
    float m_spread   = 0.0f;
    float m_presence = 0.0f;
};

FBZZ_REFLECT(CrosshairComponent)


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
    m_reportedMissingMaterial = false;

    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     下の EntityRef は空に戻る。一方 UI の GameObject は Scene 側に残っているので、
    //     無条件に組み直すとリロードのたびに照準が 1 組ずつ増えていく。
    if (!Adopt()) Build();
}

inline bool CrosshairComponent::Adopt()
{
    GameObject* canvasObject = scene.Find(CanvasName());
    if (!canvasObject) return false;

    GameObject* element = canvasObject->GetChildCount() == 1
        ? canvasObject->GetChild(0) : nullptr;

    // WHY 拾えない形なら畳むか: 矩形を並べていた頃の照準 (子 6 個) がそのまま
    //     残っていることがある。名前は同じなので、組み直しただけでは次の Adopt が
    //     どちらを拾うか決められなくなる。合わない構成は先に捨てる。
    if (!canvasObject->GetComponent<UICanvas>() || !element
        || !element->GetComponent<UIImage>()) {
        scene.Destroy(*canvasObject);
        return false;
    }

    m_canvas  = EntityRef{ canvasObject->GetID() };
    m_element = EntityRef{ element->GetID() };
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

    GameObject& element = scene.Create(kCanvasName + std::string("_Image"));
    element.runtimeGenerated = true;
    element.SetParent(canvasObject);

    UIImage& image = element.AddComponent<UIImage>();
    // 画面中央を基準に、自分の中心をそこへ合わせる。図形はこの矩形の中心から
    // 外へ伸びるので、解像度が変わっても狙点は中央に居続ける。
    image.anchoring.anchor = { 0.5f, 0.5f };
    image.anchoring.pivot  = { 0.5f, 0.5f };
    element.transform.position = Vector3::ZERO;

    m_canvas  = EntityRef{ canvasObject.GetID() };
    m_element = EntityRef{ element.GetID() };
    m_appliedSprite.clear();
}

inline float CrosshairComponent::ElementSize() const
{
    // WHY スプライトのときだけ今の開きを足すか: 絵は矩形いっぱいに引き伸ばされるので、
    //     矩形が広がることがそのまま「外へ開く」になる。
    if (UsesSprite()) return Max(spriteSize, 1.0f) + Max(m_spread, 0.0f) * 2.0f;

    // WHY 今の開きではなく最大の開きで取るか: 矩形が毎フレーム伸び縮みすると、
    //     Canvas の縮尺次第で端が半ピクセルずれ、止まっているはずの線がにじむ。
    //     図形は矩形の中で動くだけなので、開ききった大きさに固定してよい。
    const float reach = Max(Max(gap, 0.0f) + Max(emitSpread, 0.0f) + Max(tickLength, 0.0f),
                            Max(dotSize, 0.0f) * 0.5f);
    return (reach + Max(outlineWidth, 0.0f) + kEdgeMargin) * 2.0f;
}

inline void CrosshairComponent::PushMaterial(GameObject* element, const Vector4& color,
                                             bool sprite) const
{
    // 名前は UICrosshair.hlsl の MaterialConstants に宣言した変数名そのまま。
    //
    // WHY 変更検出を持たずに毎フレーム送るか: 送り先は 1 要素 10 値で、上書きは
    //     確保済みの領域へ書き戻すだけ (ScriptUIProxy)。値を止めて速くなる量より、
    //     Play 中に Inspector で触った寸法がそのまま出るほうが調整で効く。
    ui.SetMaterialColor(element, "tickColor", color);
    ui.SetMaterialColor(element, "outlineColor", outlineColor);
    ui.SetMaterialFloat(element, "gap", Max(gap, 0.0f));
    ui.SetMaterialFloat(element, "tickLength", Max(tickLength, 0.0f));
    ui.SetMaterialFloat(element, "tickThickness", Max(tickThickness, 0.0f));
    ui.SetMaterialFloat(element, "tickRoundness", Clamp01(tickRoundness));
    ui.SetMaterialFloat(element, "dotSize", Max(dotSize, 0.0f));
    ui.SetMaterialFloat(element, "spread", Max(m_spread, 0.0f));
    ui.SetMaterialFloat(element, "outlineWidth", Max(outlineWidth, 0.0f));
    ui.SetMaterialFloat(element, "spriteMode", sprite ? 1.0f : 0.0f);
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
    GameObject* element = m_element.Resolve(scene);
    if (!element) return;

    // WHY マテリアルが無いときに要素ごと止めるか: 形はシェーダーが持つので、
    //     .mat が外れた要素は「照準の大きさの白い四角」になる。画面中央がそれで
    //     塞がると、原因を探しに行く前に何も見えなくなる。出さずに 1 度だけ言う。
    if (crosshairMaterial.empty()) {
        ui.SetImageEnabled(element, false);
        if (!m_reportedMissingMaterial) {
            debug.LogError("CrosshairComponent: Crosshair Material is empty "
                           "(assign Assets/Materials/UI/UICrosshair.mat).");
            m_reportedMissingMaterial = true;
        }
        return;
    }
    ui.SetImageEnabled(element, true);
    // 同じパスなら SetMaterial 側が何もしない。付け替えは Play 中にも起こりうる。
    ui.SetMaterial(element, crosshairMaterial);

    auto* gun = Gun();
    const bool emitting = gun && (gun->IsEmitting(Polarity::Plus)
                               || gun->IsEmitting(Polarity::Minus));
    const bool drawn = !m_weaponRig || m_weaponRig->IsDrawn();

    // 開きも濃さも実時間で進める。ヒットストップ中に固まると、止めが解けた瞬間に
    // 照準だけが跳ねて、当たった手応えより先に目に入る。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    const float targetSpread = emitting ? Max(emitSpread, 0.0f) : 0.0f;
    m_spread += (targetSpread - m_spread) * (1.0f - std::exp(-Max(spreadResponse, 0.0f) * dt));

    const float targetPresence = drawn ? 1.0f : Clamp01(holsteredAlpha);
    m_presence += (targetPresence - m_presence) * (1.0f - std::exp(-Max(fadeSpeed, 0.0f) * dt));

    // WHY 変わったときだけ送るか: ResolvePath() は GUID からパスを引き直し、
    //     SetImageTexture はそのたびにテクスチャを解決する。
    const bool sprite = UsesSprite();
    if (std::string path = sprite ? crosshairSprite.ResolvePath() : std::string{};
        m_appliedSprite != path) {
        ui.SetImageTexture(element, path);
        m_appliedSprite = std::move(path);
    }

    const float size = ElementSize();
    element->transform.scale = { size, size, 1.0f };

    PushMaterial(element, CurrentColor(), sprite);
    // WHY 色ではなく濃さだけを要素へ渡すか: UIImage.color はシェーダーで最後に
    //     1 度だけ掛かる「ウィジェット全体の濃さ」で、収納中のフェードに使う。
    //     極性の色まで同じ場所へ載せると、色を触るたびにフェードの効き方が変わる。
    ui.SetImageColor(element, { 1.0f, 1.0f, 1.0f, Clamp01(m_presence) });
}

inline void CrosshairComponent::OnDestroy()
{
    // ルートに置いた以上、プレイヤーと一緒には消えない。持ち主が畳む。
    if (GameObject* canvasObject = m_canvas.Resolve(scene))
        scene.Destroy(*canvasObject);
    m_canvas  = {};
    m_element = {};
    m_appliedSprite.clear();
}

} // namespace sandbox
