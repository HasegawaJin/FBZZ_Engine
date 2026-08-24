/// @file PolarityGunHudComponent.hpp
/// @brief 左右の照射バッテリー残量を、画面下の 2 本のゲージへ流し込む
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY 左右を別々のゲージにするか:
///   企画書 11 章は左入力 = 左銃 (−) / 右入力 = 右銃 (＋) と決めている。1 本の
///   ゲージにまとめると、撃てないときにどちらが塞がっているのかが分からず、
///   3.2 の「＋の群と−の群を別々に塗る」という組み立てを画面から作れない。
///   6.3 のバッテリーは元から左右独立なので、表示もそのまま 2 本にする。
///
/// WHY 中央から外へ伸ばすか:
///   ゲージの伸びる向きが左右で揃っていると、どちらが左銃かを位置で覚え直すことになる。
///   画面中央 (照準) を境に外向きへ伸ばせば、伸びる方向そのものが入力の左右と一致する。
///
/// WHY 残量だけでなく「撃てる」を段差で出すか:
///   6.3 のバッテリーは残量が資源そのものなので長さは意味を持つが、照射中に
///   ゲージの長さを読んでいる余裕はない。空にして再点火待ちの間だけ暗く沈め、
///   撃てるようになった瞬間に色が立って一度光る、という段差を足す。
///   「もう線を引き始めてよいか」を、視線を外さずに読めるようにするため。
///
/// WHY 編集中も動かすか (FBZZ_EXECUTE_ALWAYS):
///   ゲージの見え方は位置と大きさだけでは決まらず、極の色・明るさ・電池の刻みが
///   揃って初めて読める。Play を押さないと何も出ないと、配置を 1 mm 動かすたびに
///   Play → 確認 → Stop を往復することになり、Stop でシーンが復元されるぶん
///   「今見ている絵」と「保存される値」もずれる。編集中に完成形を出す。
#pragma once

#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PolarityGunHudComponent : public Script {
    FBZZ_SCRIPT(PolarityGunHudComponent)
    FBZZ_EXECUTE_ALWAYS()

public:
    // 未設定なら名前で拾う。シーンを組み直しても既定の構成なら動く。
    FBZZ_GROUP("Left Emitter (-)")
    FBZZ_REF(GameObject, leftFill, "Fill")
    FBZZ_TOOLTIP("残量で伸びる UIImage。Fill Origin は Right (中央から左へ伸びる)")
    FBZZ_REF(GameObject, leftBackground, "Background")
    FBZZ_REF(GameObject, leftLabel, "Label")

    FBZZ_GROUP("Right Emitter (+)")
    FBZZ_REF(GameObject, rightFill, "Fill")
    FBZZ_TOOLTIP("残量で伸びる UIImage。Fill Origin は Left (中央から右へ伸びる)")
    FBZZ_REF(GameObject, rightBackground, "Background")
    FBZZ_REF(GameObject, rightLabel, "Label")

    FBZZ_GROUP("Readiness")
    FBZZ_FIELD_RANGE(float, chargingBrightness, 0.30f, "Charging Brightness", 0.0f, 1.0f)
    FBZZ_TOOLTIP("再点火できない間の明るさ。残量が戻るにつれてここから 1.0 まで上がる")
    FBZZ_FIELD_RANGE(float, readyFlashBoost, 0.9f, "Ready Flash", 0.0f, 3.0f)
    FBZZ_TOOLTIP("撃てるようになった瞬間の上乗せ。0 で光らせない")
    FBZZ_FIELD_RANGE(float, readyFlashSeconds, 0.22f, "Ready Flash Seconds", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, readyPulseDepth, 0.12f, "Ready Pulse", 0.0f, 0.5f)
    FBZZ_TOOLTIP("撃てる間の呼吸。0 で止まったゲージになる。照射中は止まる")
    FBZZ_FIELD_RANGE(float, readyPulseHz, 1.1f, "Ready Pulse Hz", 0.0f, 8.0f)

    FBZZ_GROUP("Holstered")
    // WHY 明るさではなく不透明度で消すか:
    //   最初は「位置を保ったまま暗くする」つもりで明るさだけ落としていた。
    //   ところがバッテリーは収納中も回復するので、ゲージは満タンのまま
    //   真っ黒な帯として残る。これは「使えない」ではなく「壊れている」に見える。
    //   実際 Play を始めた直後は必ず収納状態なので、開始時点で毎回それが出ていた。
    //
    //   出す情報が無いなら、薄暗く出すのではなく消すのが正しい。銃を抜いていない
    //   ときにバッテリー残量を見せる意味は無い。
    FBZZ_FIELD_RANGE(float, holsteredAlpha, 0.0f, "Holstered Alpha", 0.0f, 1.0f)
    FBZZ_TOOLTIP("収納中の不透明度。0 で完全に消える。位置の手がかりを残したいなら 0.1 前後")
    // WHY 即座に消さないか: 抜く / 収めるのたびに HUD が瞬間的に現れて消えると、
    //     視界の端で明滅として拾われて動作そのものより目立つ。
    FBZZ_FIELD_RANGE(float, holsterFadeSpeed, 9.0f, "Fade Speed", 1.0f, 40.0f)

    FBZZ_GROUP("Material")
    // WHY スクリプトから .mat を割り当てるか:
    //   電池の形はシェーダーが描くので、マテリアルが無いと UIImage はただの
    //   四角い帯になる。シーン側の割り当て漏れが「HUD の形が違う」という形で
    //   出るのは原因に辿り着きにくい。既定を持たせて、空欄のときだけ
    //   シーンの割り当てを尊重する。
    FBZZ_FIELD_FILE(batteryMaterial, "Assets/Materials/UI/UIBattery.mat",
                    "Battery Material", ".mat")
    FBZZ_TOOLTIP("左右のゲージへ割り当てる UI マテリアル。空欄でシーンの設定をそのまま使う")
    // 名前は .mat のシェーダー変数名そのまま。空欄なら送らない。
    // マテリアルが割り当たっていない要素では溜まるだけで何も描かない。
    FBZZ_FIELD(std::string, materialFillParam, "fillRatio", "Fill Ratio Param")
    FBZZ_FIELD(std::string, materialColorParam, "fillColor", "Fill Color Param")
    FBZZ_FIELD(std::string, materialFromRightParam, "fillFromRight", "Fill From Right Param")
    FBZZ_TOOLTIP("左銃を中央から外向きへ伸ばすための向き指定。1 = 右詰め")
    FBZZ_FIELD(std::string, materialDepletedParam, "depleted", "Depleted Param")
    FBZZ_TOOLTIP("6.3 の再点火待ち。1 で『量は戻っているがまだ使えない』見た目になる")

    FBZZ_GROUP("Background")
    FBZZ_FIELD_COLOR(backgroundColor, (Vector4{ 0.02f, 0.02f, 0.03f, 0.78f }), "Color")
    FBZZ_TOOLTIP("下地は極の色を薄く混ぜる。どちら側のゲージかが空でも読める")
    FBZZ_FIELD_RANGE(float, backgroundTint, 0.12f, "Polarity Tint", 0.0f, 1.0f)

    // 編集中に流し込む値。Play では PlayerComponent が同じ場所へ入るので使われない。
    //
    // WHY PlayerComponent を読まないか: 編集中は PlayerComponent の初期化が走らず、
    //     必須 fzdata を束ねる前の状態で残っている。その BatteryOf() は未設定の
    //     tuning を辿るため、読んだ瞬間に落ちる。値の出所を編集中だけ差し替える。
    FBZZ_GROUP("Edit Mode Preview")
    FBZZ_FIELD_RANGE(float, previewCharge, 0.62f, "Charge", 0.0f, 1.0f)
    FBZZ_TOOLTIP("編集中に出す残量。満タンと空だけでなく途中も確かめられるよう既定は半端な値")
    FBZZ_FIELD(bool, previewReady, true, "Ready")
    FBZZ_TOOLTIP("撃てる状態として出す。切ると再点火待ちの沈んだ見え方になる")
    FBZZ_FIELD(bool, previewEmitting, false, "Emitting")
    FBZZ_FIELD(bool, previewDrawn, true, "Weapons Drawn")
    FBZZ_TOOLTIP("切ると収納中の扱いになり、Holstered Alpha のとおり消える")
    // WHY 既定で動かさないか: 呼吸や発光を編集中も回すと、ゲージが毎フレーム
    //     別の色を書き込み、シーンが延々と変化し続ける (未保存マークが常時点灯する)。
    //     止めておけば 1 度書いた後は同じ値に落ち着く。呼吸そのものを調整する
    //     ときだけ入れる。
    FBZZ_FIELD(bool, previewAnimate, false, "Animate")
    FBZZ_TOOLTIP("編集中も立ち上がりの発光・呼吸・出し入れのフェードを動かす")

    void OnStart() override;
    void OnLateUpdate() override;

private:
    /// ゲージ 1 本を描くのに要る状態。Play は PlayerComponent、編集中は preview から作る。
    ///
    /// WHY PlayerComponent を直接渡さないか: 値の出所が 2 つになっても描き方は同じ。
    ///     間に 1 枚挟むと、描画側は「どちらから来た値か」を一切知らずに済む。
    struct SideState {
        float charge   = 1.0f;
        bool  ready    = true;
        bool  emitting = false;
        bool  drawn    = true;
        /// 時間で動く要素 (立ち上がりの発光・呼吸・出し入れのフェード) を使わず、
        /// 落ち着いた最終形だけを出す。
        bool  settled  = false;
    };

    /// 片側ぶんの参照。左右で同じ処理を 2 度書かないためにまとめる。
    struct Side {
        EntityRef fill;
        EntityRef background;
        EntityRef label;
        // 前フレームに撃てたか。光らせるのは「撃てるようになった瞬間」だけ。
        bool  wasReady = false;
        float flashRemaining = 0.0f;
        // 表示の濃さ 0..1。収納で 0、抜いて 1 へ。明滅を避けるため補間する。
        float presence = 0.0f;
    };

    static constexpr const char* kLeftFillName        = "HUD_GunLeftFill";
    static constexpr const char* kLeftBackgroundName  = "HUD_GunLeftBackground";
    static constexpr const char* kLeftLabelName       = "HUD_GunLeftLabel";
    static constexpr const char* kRightFillName       = "HUD_GunRightFill";
    static constexpr const char* kRightBackgroundName = "HUD_GunRightBackground";
    static constexpr const char* kRightLabelName      = "HUD_GunRightLabel";

    [[nodiscard]] GameObject* Resolve(const Ref<GameObject>& reference, const char* name) const;
    void BindSide(Side& side, const Ref<GameObject>& fillRef, const char* fillName,
                  const Ref<GameObject>& backgroundRef, const char* backgroundName,
                  const Ref<GameObject>& labelRef, const char* labelName);
    [[nodiscard]] SideState StateOf(const PlayerComponent& player, Polarity polarity) const;
    [[nodiscard]] SideState PreviewState() const;
    void RefreshSide(Side& side, Polarity polarity, const SideState& state, float dt);

    Side m_left;
    Side m_right;
};

FBZZ_REFLECT(PolarityGunHudComponent)

inline GameObject* PolarityGunHudComponent::Resolve(const Ref<GameObject>& reference,
                                                    const char* name) const
{
    if (GameObject* object = reference.Get())
        return object;
    return scene.Find(name);
}

inline void PolarityGunHudComponent::BindSide(
    Side& side,
    const Ref<GameObject>& fillRef, const char* fillName,
    const Ref<GameObject>& backgroundRef, const char* backgroundName,
    const Ref<GameObject>& labelRef, const char* labelName)
{
    side = {};

    // 参照の解決は Play 開始時に 1 度だけ。毎フレーム名前で探すと、見つからない構成で
    // 静かに全シーン走査を続けることになる。
    GameObject* fill = Resolve(fillRef, fillName);
    if (!fill) {
        debug.LogError(std::string("PolarityGunHudComponent: gauge fill not found (assign it, "
                                   "or name it ") + fillName + " in the scene).");
        return;
    }
    side.fill = EntityRef{ fill->GetID() };

    if (GameObject* background = Resolve(backgroundRef, backgroundName))
        side.background = EntityRef{ background->GetID() };
    if (GameObject* label = Resolve(labelRef, labelName))
        side.label = EntityRef{ label->GetID() };
}

inline void PolarityGunHudComponent::OnStart()
{
    // WHY 編集中は PlayerComponent が無くても続けるか: 出所が無いのは値だけで、
    //     ゲージの組み立て自体は進められる。ここで抜けると編集中は何も出ないままになる。
    if (app.IsPlaying() && !scene.GetScript<PlayerComponent>()) {
        debug.LogError("PolarityGunHudComponent requires PlayerComponent on the same object.");
        return;
    }

    BindSide(m_left, leftFill, kLeftFillName, leftBackground, kLeftBackgroundName,
             leftLabel, kLeftLabelName);
    BindSide(m_right, rightFill, kRightFillName, rightBackground, kRightBackgroundName,
             rightLabel, kRightLabelName);

    // 電池の形はマテリアルが描く。割り当ては 1 度でよい (毎フレーム送るのは値だけ)。
    if (!batteryMaterial.empty()) {
        ui.SetMaterial(m_left.fill.Resolve(scene), batteryMaterial);
        ui.SetMaterial(m_right.fill.Resolve(scene), batteryMaterial);
    }

    // 記号は 12.3 の二重表現。文字列をシーンへ直接書くと、配色と記号の対応が
    // PolarityTypes.hpp の外にもう 1 部できてしまう。
    ui.SetText(m_left.label.Resolve(scene), PolaritySymbol(Polarity::Minus));
    ui.SetText(m_right.label.Resolve(scene), PolaritySymbol(Polarity::Plus));
}

inline PolarityGunHudComponent::SideState
PolarityGunHudComponent::StateOf(const PlayerComponent& player, Polarity polarity) const
{
    SideState state;
    state.charge   = player.BatteryOf(polarity);
    state.ready    = player.CanEmit(polarity);
    state.emitting = player.IsEmitting(polarity);
    state.drawn    = player.AreWeaponsDrawn();
    return state;
}

inline PolarityGunHudComponent::SideState PolarityGunHudComponent::PreviewState() const
{
    SideState state;
    state.charge   = previewCharge;
    state.ready    = previewReady;
    state.emitting = previewEmitting;
    state.drawn    = previewDrawn;
    state.settled  = !previewAnimate;
    return state;
}

inline void PolarityGunHudComponent::RefreshSide(Side& side, Polarity polarity,
                                                 const SideState& state, float dt)
{
    GameObject* fill = side.fill.Resolve(scene);
    if (!fill) return;

    const float charge = Clamp01(state.charge);

    // 光らせるのは立ち上がりだけ。撃てる間ずっと光らせると、光っていることが
    // 状態ではなく背景になり、次に撃てるようになった瞬間が見えなくなる。
    if (state.settled) {
        side.wasReady       = state.ready;
        side.flashRemaining = 0.0f;
    } else {
        if (state.ready && !side.wasReady)
            side.flashRemaining = std::max(readyFlashSeconds, 0.0f);
        side.wasReady = state.ready;
        side.flashRemaining = std::max(0.0f, side.flashRemaining - dt);
    }

    // 撃てる間は明るく、空にして再点火を待つ間は暗い。残量そのものはゲージの
    // 長さが言うので、明るさは「今引き始められるか」の 1 点だけに使う。
    float brightness = state.ready ? 1.0f : Lerp(Clamp01(chargingBrightness), 1.0f, charge);
    if (readyFlashSeconds > 0.0f && side.flashRemaining > 0.0f) {
        const float falloff = Clamp01(side.flashRemaining / readyFlashSeconds);
        brightness += readyFlashBoost * falloff * falloff;
    }
    // 照射中は呼吸を止める。減っていくゲージの上で明るさまで揺れると、
    // 残量が減っているのか呼吸で沈んだだけなのかが読めない。
    if (!state.settled && state.ready && !state.emitting && readyPulseHz > 0.0f)
        brightness += std::sin(Time::unscaledTime * readyPulseHz * TWO_PI) * readyPulseDepth;
    // 呼吸で下振れした分が負へ回ると色が反転する。下だけ止める (上は光らせたい)。
    brightness = Max(brightness, 0.0f);

    // 収納中は「暗い」ではなく「無い」。出す情報が無いので消す。
    const float target = state.drawn ? 1.0f : Clamp01(holsteredAlpha);
    if (state.settled) {
        side.presence = target;
    } else {
        const float fade = 1.0f - std::exp(-Max(holsterFadeSpeed, 0.0f) * dt);
        side.presence += (target - side.presence) * fade;
    }
    const float presence = Clamp01(side.presence);

    const Vector4 base = PolarityColor(polarity);
    // WHY 明るさは RGB、出し入れはアルファか:
    //   充填量の読み取りは明るさで表す (透けさせると背景の明暗で読みが変わる)。
    //   一方「そもそも表示するか」は濃さの問題で、RGB を落とすと満タンのゲージが
    //   黒い帯として残ってしまう。役割の違う 2 つを別のチャンネルへ割り当てる。
    const Vector4 color{ base.x * brightness, base.y * brightness, base.z * brightness,
                         base.w * presence };

    const bool usesMaterial = !batteryMaterial.empty();

    // WHY マテリアルがあるとき fillAmount を 1 に固定するか:
    //   UISystem の fillAmount は矩形ごと切り落とす。電池の外殻と端子まで一緒に
    //   消えてしまい、しかもシェーダー側の fillRatio と二重に効く。
    //   形をシェーダーが描く以上、残量の解釈もシェーダーに一本化する。
    ui.SetImageFillAmount(fill, usesMaterial ? 1.0f : charge);

    // WHY マテリアルのときは白 + 濃さだけを送るか:
    //   UIBattery は g_Color を「ウィジェット全体の濃さ」として最後に 1 度だけ掛ける。
    //   ここへ明るさまで載せると fillColor と二重になり、明るさを触るたびに
    //   収納フェードの効き方まで変わる。明るさは fillColor へ、濃さは color へ。
    const Vector4 imageColor = usesMaterial
        ? Vector4{ 1.0f, 1.0f, 1.0f, presence }
        : color;
    ui.SetImageColor(fill, imageColor);
    ui.SetTextColor(side.label.Resolve(scene), color);

    // マテリアル側へ状態を流す。UIBattery.hlsl を割り当てた要素では、
    // 電池の輪郭・セルの刻み・先端の発光・再点火待ちの見え方までが .mat の担当になる。
    if (!materialFillParam.empty())
        ui.SetMaterialFloat(fill, materialFillParam, charge);
    if (!materialColorParam.empty())
        ui.SetMaterialColor(fill, materialColorParam, color);
    // 伸びる向きは左右で違うが、.mat は左右で共有したい。
    // 向きだけを要素ごとの上書きにすれば .mat は 1 枚で済む。
    if (!materialFromRightParam.empty())
        ui.SetMaterialFloat(fill, materialFromRightParam,
                            polarity == Polarity::Minus ? 1.0f : 0.0f);
    // 空にして回復を待っている間 (6.3 の再点火待ち)。残量だけでは
    // 「溜まっているのに撃てない」が読めないので、状態そのものを送る。
    if (!materialDepletedParam.empty())
        ui.SetMaterialFloat(fill, materialDepletedParam,
                            (!state.ready && charge > 0.0f) ? 1.0f : 0.0f);

    // 下地も一緒に消す。溝だけ残ると「入るはずの物が入っていない」に見える。
    if (GameObject* background = side.background.Resolve(scene)) {
        const float tint = Clamp01(backgroundTint);
        Vector4 tinted = backgroundColor + (base - backgroundColor) * tint;
        tinted.w = backgroundColor.w * presence;
        ui.SetImageColor(background, tinted);
    }
}

inline void PolarityGunHudComponent::OnLateUpdate()
{
    // 光りと呼吸は実時間で進める。ヒットストップ中に固まると、止めが解けた瞬間に
    // 撃てるようになったのか、止まっている間に満ちていたのかが分からなくなる。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    // 編集中は PlayerComponent へ一切触らない。初期化前の tuning を辿って落ちる。
    if (app.IsEditMode()) {
        const SideState preview = PreviewState();
        RefreshSide(m_left, Polarity::Minus, preview, dt);
        RefreshSide(m_right, Polarity::Plus, preview, dt);
        return;
    }

    const auto* player = scene.GetScript<PlayerComponent>();
    // WHY enabled を見るか: PlayerComponent は必須 fzdata が無いと自分を無効化する。
    //     その状態の BatteryOf() は未設定の tuning を辿るため、読んではいけない。
    if (!player || !player->enabled) return;

    RefreshSide(m_left, Polarity::Minus, StateOf(*player, Polarity::Minus), dt);
    RefreshSide(m_right, Polarity::Plus, StateOf(*player, Polarity::Plus), dt);
}

} // namespace sandbox
