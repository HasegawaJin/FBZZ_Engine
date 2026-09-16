/// @file    BossLegHealthBarComponent.hpp
/// @brief   ボスの脚 4 本それぞれに、残り耐久の WorldSpace バーを追従させる
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY 画面上部のボスバーと別に要るか:
///   ボス本体の HP は «あとどれだけ殴れば終わるか» を言う。脚の耐久は
///   «この脚をあと何回引き合いに使えるか» で、意味も置き場所も違う。1 本の
///   バーへ混ぜると、どちらが減ったのか画面から読めない。
///
/// WHY 脚のそばに出すか (HUD へ 4 本並べず):
///   プレイヤーが選ぶのは «どの 2 本で組むか» で、その判断は脚を見ながら行う。
///   画面の隅に 4 本並べると、盤面と HUD を往復しないと選べなくなる。
///
/// WHY BossRigComponent に同居させないか:
///   あちらは «引き合わせて転倒させる» 仕組みで、こちらは «その状態を絵にする» 側。
///   混ぜると、バーの見た目を触るたびに転倒の判定が載ったファイルを開くことになる。
///   状態はリグの公開 API (LegDurabilityRatio / LegAnchor) からだけ読む。
#pragma once

#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossRigComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class BossLegHealthBarComponent : public Script {
    FBZZ_SCRIPT(BossLegHealthBarComponent)

public:
    FBZZ_GROUP("配置")
    FBZZ_FIELD_RANGE(float, headroom, 0.55f, "Headroom", 0.0f, 5.0f)
    FBZZ_TOOLTIP("膝下の当たり判定の中心からバーまでのワールド高さ。"
                 "上げすぎると胴体に隠れ、下げすぎると床に埋まる")
    FBZZ_FIELD_RANGE(float, barWidth, 0.90f, "Bar Width", 0.05f, 6.0f)
    FBZZ_FIELD_RANGE(float, barHeight, 0.13f, "Bar Height", 0.02f, 2.0f)
    FBZZ_FIELD_RANGE(float, borderWidth, 0.02f, "Border Width", 0.0f, 0.5f)

    FBZZ_GROUP("Color")
    FBZZ_FIELD_COLOR(backgroundColor, (Vector4{ 0.02f, 0.02f, 0.03f, 0.78f }), "Background")
    // WHY 無彩色に置くか: 赤青は «どちらの刀か» に割り当ててある (BladeColors.hpp)。
    //     脚のバーがその 2 色を出すと、プレイヤーの刀の色と同じ語で喋ることになる。
    FBZZ_FIELD_COLOR(neutralColor, (Vector4{ 0.72f, 0.76f, 0.80f, 1.0f }), "Neutral")

    FBZZ_GROUP("Visibility")
    // WHY 無傷のうちは隠すか: 4 本とも満タンのバーが常時出ていると、盤面に情報が
    //     4 つ増えるだけで «どれを使ったか» が読めない。減った脚だけが名乗り出る。
    FBZZ_FIELD(bool, hideWhenFull, true, "Hide When Full")
    FBZZ_TOOLTIP("無傷の脚のバーを隠す。切ると 4 本とも常時出る")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugVisible, 0, "Visible Bars")

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    // Canvas のピクセル座標系とワールド単位の換算比。UICanvas::worldScale の逆数。
    // ボス本体のバーと同じ値に揃える ─ 別の比にすると、同じ Bar Width の
    // 数字が敵とボスで違う大きさになる。
    static constexpr float kPixelsPerUnit = 100.0f;

    [[nodiscard]] BossRigComponent* Rig() const
    { return scene.GetScript<BossRigComponent>(); }

    /// 脚 1 本ぶんのバー名。DLL リロードで拾い直せるよう、所有者と脚で一意にする。
    [[nodiscard]] std::string CanvasName(const GameObject& owner, int leg) const;

    /// 生成済みのバーを拾い直せたら true。スクリプト DLL のリロード対策。
    bool Adopt(GameObject& owner);
    void Build(GameObject& owner);
    void LayoutOne(int leg, const Vector3& anchor);

    struct Bar {
        EntityRef canvas;
        EntityRef background;
        EntityRef fill;
    };

    Bar m_bars[4];
};

FBZZ_REFLECT(BossLegHealthBarComponent)

inline std::string BossLegHealthBarComponent::CanvasName(const GameObject& owner, int leg) const
{
    return "BossLegBar_" + owner.instanceId + "_" + std::to_string(leg);
}

inline bool BossLegHealthBarComponent::Adopt(GameObject& owner)
{
    for (int leg = 0; leg < BossRigComponent::LegCount(); ++leg) {
        GameObject* canvasObject = scene.Find(CanvasName(owner, leg));
        if (!canvasObject || !canvasObject->GetComponent<UICanvas>()) return false;

        m_bars[leg].canvas = EntityRef{ canvasObject->GetID() };
        m_bars[leg].background = {};
        m_bars[leg].fill = {};
        for (int i = 0; i < canvasObject->GetChildCount(); ++i) {
            GameObject* child = canvasObject->GetChild(i);
            if (!child) continue;
            if (child->name.find("Background") != std::string::npos)
                m_bars[leg].background = EntityRef{ child->GetID() };
            else
                m_bars[leg].fill = EntityRef{ child->GetID() };
        }
        if (!m_bars[leg].background.Resolve(scene) || !m_bars[leg].fill.Resolve(scene))
            return false;
    }
    return true;
}

inline void BossLegHealthBarComponent::Build(GameObject& owner)
{
    // WHY ボスの子にしないか: 子にすると位置がローカル値になり、ボスの回転と
    //     TransformSystem の巡回順が «バーがどこに出るか» に効く。ルートに置いて
    //     ワールド座標を直接書けば、位置は LegAnchor() の 1 式だけで決まる
    //     (ボス本体のバーと同じ理由)。
    //
    // WHY 4 本ぶんを作り切ってから参照を取るか: scene.Create は GameObject 配列を
    //     再確保する。作りながら掴んだポインタは次の Create で無効になる。
    for (int leg = 0; leg < BossRigComponent::LegCount(); ++leg) {
        const std::string name = CanvasName(owner, leg);

        GameObject& canvasObject = scene.Create(name);
        canvasObject.runtimeGenerated = true;
        UICanvas& canvas  = canvasObject.AddComponent<UICanvas>();
        canvas.renderMode = UIRenderMode::WorldSpace;
        canvas.faceCamera = true;
        canvas.worldScale = 1.0f / kPixelsPerUnit;
        m_bars[leg].canvas = EntityRef{ canvasObject.GetID() };
    }

    for (int leg = 0; leg < BossRigComponent::LegCount(); ++leg) {
        GameObject* canvasObject = m_bars[leg].canvas.Resolve(scene);
        if (!canvasObject) continue;

        GameObject& backgroundObject = scene.Create("BossLegBar_Background");
        backgroundObject.runtimeGenerated = true;
        backgroundObject.AddComponent<UIImage>().sortOrder = 0;
        m_bars[leg].background = EntityRef{ backgroundObject.GetID() };

        GameObject& fillObject = scene.Create("BossLegBar_Fill");
        fillObject.runtimeGenerated = true;
        UIImage& fill   = fillObject.AddComponent<UIImage>();
        fill.sortOrder  = 1;
        fill.fillOrigin = UIImageFillOrigin::Left;
        m_bars[leg].fill = EntityRef{ fillObject.GetID() };
    }

    // 親付けは全部作り終えてから。SetParent はワールド姿勢を保つ実装でも、
    // 生成の途中で掴んだ参照は無効になりうる。
    for (int leg = 0; leg < BossRigComponent::LegCount(); ++leg) {
        GameObject* canvasObject = m_bars[leg].canvas.Resolve(scene);
        GameObject* background   = m_bars[leg].background.Resolve(scene);
        GameObject* fillObject   = m_bars[leg].fill.Resolve(scene);
        if (!canvasObject || !background || !fillObject) continue;
        background->SetParent(*canvasObject);
        fillObject->SetParent(*canvasObject);
    }
}

inline void BossLegHealthBarComponent::LayoutOne(int leg, const Vector3& anchor)
{
    GameObject* canvasObject = m_bars[leg].canvas.Resolve(scene);
    GameObject* background   = m_bars[leg].background.Resolve(scene);
    GameObject* fillObject   = m_bars[leg].fill.Resolve(scene);
    if (!canvasObject || !background || !fillObject) return;

    const float width  = std::max(barWidth, 0.05f) * kPixelsPerUnit;
    const float height = std::max(barHeight, 0.02f) * kPixelsPerUnit;
    const float border = Clamp(borderWidth * kPixelsPerUnit, 0.0f,
                               std::min(width, height) * 0.4f);

    if (auto* canvas = canvasObject->GetComponent<UICanvas>()) {
        canvas->canvasWidth  = width;
        canvas->canvasHeight = height;
    }

    // Canvas はルートなので local = world。この Layout は LateUpdate で走り、
    // UI の描画はその後なので、world も書いておけば 1 フレーム遅れない。
    canvasObject->transform.position      = anchor;
    canvasObject->transform.worldPosition = anchor;

    // UI 要素の localScale.xy は倍率ではなく Canvas ピクセル単位の幅・高さ。
    background->transform.position = Vector3::ZERO;
    background->transform.scale    = { width, height, 1.0f };
    fillObject->transform.position = { border, border, 0.0f };
    fillObject->transform.scale    = { width - border * 2.0f, height - border * 2.0f, 1.0f };
}

inline void BossLegHealthBarComponent::OnStart()
{
    GameObject* owner = scene.Self();
    if (!owner) return;

    if (!Rig()) {
        debug.LogError("BossLegHealthBarComponent requires BossRigComponent "
                       "on the same object (it owns the legs and their durability).");
        enabled = false;
        return;
    }

    // WHY 先に拾い直すか: DLL をリロードするとこの Script は作り直され、EntityRef は
    //     空に戻る。一方バーの GameObject は Scene に残るので、無条件に組み直すと
    //     リロードのたびにバーが 4 本ずつ増える。
    if (!Adopt(*owner))
        Build(*owner);
}

inline void BossLegHealthBarComponent::OnLateUpdate()
{
    const auto* rig = Rig();
    if (!rig) return;

    debugVisible = 0;

    for (int leg = 0; leg < BossRigComponent::LegCount(); ++leg) {
        GameObject* canvasObject = m_bars[leg].canvas.Resolve(scene);
        if (!canvasObject) continue;

        const float ratio = rig->LegDurabilityRatio(leg);
        Vector3     anchor;

        // 落ちた脚と、無傷で隠す設定の脚は畳む。位置も更新しない ─ もぎ取った後の
        // 当たり判定はボーンに付いたまま動くので、バーだけが空中に残る。
        const bool show = !rig->IsLegBroken(leg)
                       && (!hideWhenFull || ratio < 1.0f)
                       && rig->LegAnchor(leg, anchor);
        canvasObject->SetActive(show);
        if (!show) continue;

        anchor.y += headroom;
        LayoutOne(leg, anchor);

        Vector4 color = neutralColor;
        color.w = 1.0f;

        ui.SetImageColor(m_bars[leg].background.Resolve(scene), backgroundColor);
        ui.SetImageColor(m_bars[leg].fill.Resolve(scene), color);
        ui.SetImageFillAmount(m_bars[leg].fill.Resolve(scene), Clamp01(ratio));
        ++debugVisible;
    }
}

inline void BossLegHealthBarComponent::OnDestroy()
{
    // ランタイム生成なのでシーンには残らないが、Play を止めた瞬間にボスが消えても
    // バーだけがアリーナに残る経路がある。持ち主が畳まれたら一緒に畳む。
    for (Bar& bar : m_bars) {
        if (GameObject* canvasObject = bar.canvas.Resolve(scene))
            scene.Destroy(*canvasObject);
        bar = {};
    }
}

} // namespace sandbox
