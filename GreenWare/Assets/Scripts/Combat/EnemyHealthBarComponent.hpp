/// @file    EnemyHealthBarComponent.hpp
/// @brief   敵の頭上に追従する WorldSpace 体力バー
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY EnemyHealthComponent に同居させないか:
///   体力の値と、その値の見せ方は寿命が違う。バーの見た目 (太さ・色・高さ) は
///   絵合わせで何度も触るが、ダメージ式は企画の判断が変わったときにしか触らない。
///   同じクラスに置くと、色を 1 つ変えたいだけで被弾処理を読む羽目になる。
///
/// WHY UI をシーンへ置かずランタイムで組むか:
///   バーは背景と前景の 2 枚 + Canvas の 3 GameObject でできている。これをシーンへ
///   保存すると、敵を 1 体増やすたびに階層へ 3 行増え、しかも太さを変える作業が
///   「敵の数だけ同じ数値を入れ直す」ことになる。runtimeGenerated で組めば
///   シーンに残るのはこのスクリプト 1 行だけで、寸法の出所も 1 箇所に閉じる。
#pragma once

#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EnemyHealthBarComponent : public Script {
    FBZZ_SCRIPT(EnemyHealthBarComponent)

public:
    FBZZ_GROUP("Layout")
    FBZZ_FIELD_RANGE(float, headroom, 0.35f, "Headroom", 0.0f, 5.0f)
    FBZZ_TOOLTIP("体の上端からバー中心までのワールド距離。上端はコライダーから測るので、"
                 "敵の大きさやスケールを変えても入れ直さなくてよい")
    FBZZ_FIELD_RANGE(float, fallbackBodyHeight, 1.0f, "Fallback Body Height", 0.0f, 10.0f)
    FBZZ_TOOLTIP("コライダーが無く上端を測れないときに、原点からの高さとして使う値")
    FBZZ_FIELD_RANGE(float, barWidth, 1.2f, "Bar Width", 0.05f, 10.0f)
    FBZZ_FIELD_RANGE(float, barHeight, 0.16f, "Bar Height", 0.02f, 2.0f)
    FBZZ_FIELD_RANGE(float, borderWidth, 0.02f, "Border Width", 0.0f, 0.5f)

    FBZZ_GROUP("Color")
    FBZZ_FIELD_COLOR(backgroundColor, (Vector4{ 0.02f, 0.02f, 0.03f, 0.78f }), "Background")
    FBZZ_FIELD_COLOR(fullColor,       (Vector4{ 0.30f, 0.92f, 0.40f, 1.00f }), "Full")
    FBZZ_FIELD_COLOR(emptyColor,      (Vector4{ 0.85f, 0.35f, 0.20f, 1.00f }), "Empty")
    FBZZ_TOOLTIP("残量ゼロ側の色。Full と同じ色にすれば、長さの変化だけで読ませられる")

    FBZZ_GROUP("Drain")
    // WHY 遅れて減る帯を出すか: 3 発で撃破される相手だと、当たった瞬間に長さが飛ぶだけで、
    //     「今どれだけ削れたか」が絵として残らない。少し置いてから追いかける帯があると、
    //     削れた量そのものが数フレーム画面に残り、減っている実感になる。
    FBZZ_FIELD_COLOR(drainColor, (Vector4{ 1.00f, 0.95f, 0.75f, 1.00f }), "Drain Color")
    FBZZ_FIELD_RANGE(float, drainHold, 0.18f, "Drain Hold", 0.0f, 2.0f)
    FBZZ_TOOLTIP("被弾してから追従を始めるまでの秒数。0 で即座に追いつく")
    FBZZ_FIELD_RANGE(float, drainSpeed, 0.9f, "Drain Speed", 0.05f, 10.0f)
    FBZZ_TOOLTIP("追従の速さ (残量割合 / 秒)。1.0 なら満タンから空まで 1 秒かけて追いつく")

    FBZZ_GROUP("Visibility")
    FBZZ_FIELD(bool, hideWhenFull, false, "Hide When Full")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawAnchor, false, "Draw Anchor")
    FBZZ_TOOLTIP("バーを置く点と体の上端を線で描く。位置が合わないときの切り分け用")
    FBZZ_FIELD_READ_ONLY(float, debugAnchorHeight, 0.0f, "Anchor Height")
    FBZZ_TOOLTIP("敵の原点からバーまでのワールド高さ")

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    // Canvas のピクセル座標系とワールド単位の換算比。UICanvas::worldScale の逆数。
    static constexpr float kPixelsPerUnit = 100.0f;

    static constexpr const char* kCanvasName     = "HealthBar";
    static constexpr const char* kBackgroundName = "HealthBar_Background";
    static constexpr const char* kDrainName      = "HealthBar_Drain";
    static constexpr const char* kFillName       = "HealthBar_Fill";

    // 体の上端の高さ。コライダーから測り、Transform のスケールを掛けたワールド値。
    [[nodiscard]] float BodyTopWorld(GameObject& owner) const;
    // バー中心を置くワールド座標。
    [[nodiscard]] Vector3 AnchorPoint(GameObject& owner) const;
    // このスクリプトが持つバーの Canvas 名。敵ごとに一意にする。
    [[nodiscard]] std::string CanvasName(const GameObject& owner) const;

    // 生成済みのバーを拾い直せたら true。スクリプト DLL のリロード対策。
    bool Adopt(GameObject& owner);
    void Build(GameObject& owner);
    void Layout();
    // 遅れて追いつく帯の残量を進める。戻り値が帯の塗り潰し量。
    float AdvanceDrain(float ratio);

    EntityRef m_canvas;
    EntityRef m_background;
    EntityRef m_drain;
    EntityRef m_fill;

    // 追従帯の現在値と、被弾を検出するための前フレームの残量。
    float m_drainRatio = 1.0f;
    float m_lastRatio  = 1.0f;
    float m_holdRemaining  = 0.0f;
};

FBZZ_REFLECT(EnemyHealthBarComponent)

inline void EnemyHealthBarComponent::OnStart()
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    if (!health) {
        debug.LogError("EnemyHealthBarComponent requires EnemyHealthComponent on the same object.");
        return;
    }

    GameObject* owner = scene.Self();
    if (!owner) return;

    // 追従帯は現在値から始める。DLL リロードで戦闘中に作り直されても、
    // 満タンから今の残量まで一度落ちる、という嘘のアニメーションを出さない。
    m_drainRatio    = health->Normalized();
    m_lastRatio     = m_drainRatio;
    m_holdRemaining = 0.0f;

    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     下の EntityRef は空に戻る。一方バーの GameObject は Scene 側に残っているため、
    //     無条件に組み直すとリロードのたびにバーが 1 組ずつ増えていく。
    if (!Adopt(*owner))
        Build(*owner);

    Layout();
}

inline std::string EnemyHealthBarComponent::CanvasName(const GameObject& owner) const
{
    // WHY 名前を敵ごとに変えるか: バーはルートに置くので、名前で拾い直すときに
    //     同名だと最初の 1 体ぶんしか見つからず、2 体目以降が毎リロード増える。
    return std::string(kCanvasName) + "_" + owner.instanceId;
}

inline bool EnemyHealthBarComponent::Adopt(GameObject& owner)
{
    GameObject* canvasObject = scene.Find(CanvasName(owner));
    if (!canvasObject || !canvasObject->GetComponent<UICanvas>()) return false;

    EntityRef background{};
    EntityRef drain{};
    EntityRef fill{};
    for (int i = 0; i < canvasObject->GetChildCount(); ++i) {
        GameObject* child = canvasObject->GetChild(i);
        if (!child) continue;
        if (child->name == kBackgroundName)   background = EntityRef{ child->GetID() };
        else if (child->name == kDrainName)   drain      = EntityRef{ child->GetID() };
        else if (child->name == kFillName)    fill       = EntityRef{ child->GetID() };
    }
    if (!background.IsValid() || !drain.IsValid() || !fill.IsValid()) return false;

    m_canvas     = EntityRef{ canvasObject->GetID() };
    m_background = background;
    m_drain      = drain;
    m_fill       = fill;
    return true;
}

inline void EnemyHealthBarComponent::Build(GameObject& owner)
{
    // WHY 敵の子にしないか:
    //   子にすると位置がローカル値になり、親のスケール・回転・TransformSystem の巡回順が
    //   全部「バーがどこに出るか」に効いてくる。敵は scale 2 の剛体で物理補間も掛かるため、
    //   ずれたときに原因の切り分けができない。ルートに置いてワールド座標を直接書けば、
    //   バーの位置は AnchorPoint() の 1 式だけで決まる。
    GameObject& canvasObject = scene.Create(CanvasName(owner));
    canvasObject.runtimeGenerated = true;
    UICanvas& canvas = canvasObject.AddComponent<UICanvas>();
    canvas.renderMode = UIRenderMode::WorldSpace;
    canvas.faceCamera = true;
    canvas.worldScale = 1.0f / kPixelsPerUnit;

    GameObject& backgroundObject = scene.Create(kBackgroundName);
    backgroundObject.runtimeGenerated = true;
    backgroundObject.SetParent(canvasObject);
    backgroundObject.AddComponent<UIImage>().sortOrder = 0;

    // 遅れて追いつく帯は本体の下。削れた区間だけが本体の右側からはみ出して見える。
    GameObject& drainObject = scene.Create(kDrainName);
    drainObject.runtimeGenerated = true;
    drainObject.SetParent(canvasObject);
    UIImage& drain = drainObject.AddComponent<UIImage>();
    drain.sortOrder  = 1;
    drain.fillOrigin = UIImageFillOrigin::Left;

    GameObject& fillObject = scene.Create(kFillName);
    fillObject.runtimeGenerated = true;
    fillObject.SetParent(canvasObject);
    UIImage& fill = fillObject.AddComponent<UIImage>();
    fill.sortOrder  = 2;
    fill.fillOrigin = UIImageFillOrigin::Left;

    m_canvas     = EntityRef{ canvasObject.GetID() };
    m_background = EntityRef{ backgroundObject.GetID() };
    m_drain      = EntityRef{ drainObject.GetID() };
    m_fill       = EntityRef{ fillObject.GetID() };
}

inline float EnemyHealthBarComponent::BodyTopWorld(GameObject& owner) const
{
    return bodybounds::TopWorld(owner, fallbackBodyHeight);
}

inline Vector3 EnemyHealthBarComponent::AnchorPoint(GameObject& owner) const
{
    Vector3 anchor = owner.transform.worldPosition;
    anchor.y += BodyTopWorld(owner) + headroom;
    return anchor;
}

inline void EnemyHealthBarComponent::Layout()
{
    GameObject* owner            = scene.Self();
    GameObject* canvasObject     = m_canvas.Resolve(scene);
    GameObject* backgroundObject = m_background.Resolve(scene);
    GameObject* drainObject      = m_drain.Resolve(scene);
    GameObject* fillObject       = m_fill.Resolve(scene);
    if (!owner || !canvasObject || !backgroundObject || !drainObject || !fillObject) return;

    const float width  = std::max(barWidth, 0.05f) * kPixelsPerUnit;
    const float height = std::max(barHeight, 0.02f) * kPixelsPerUnit;
    // 枠が太すぎて前景が消えると、満タンと空の区別が付かなくなる。
    const float border = Clamp(borderWidth * kPixelsPerUnit, 0.0f, std::min(width, height) * 0.4f);

    if (auto* canvas = canvasObject->GetComponent<UICanvas>()) {
        canvas->canvasWidth  = width;
        canvas->canvasHeight = height;
    }

    // Canvas はルートなので local = world。割り戻しも回転の継承も挟まらない。
    // WHY world 値まで書くか: ルートの world 値は TransformSystem が local をそのまま
    //     複製するだけなので、両方書いても食い違わない。この Layout は LateScript で走り、
    //     UI の描画はそのあとなので、world を書いておけば 1 フレーム遅れずに反映される。
    const Vector3 anchor = AnchorPoint(*owner);
    canvasObject->transform.position      = anchor;
    canvasObject->transform.worldPosition = anchor;
    debugAnchorHeight = anchor.y - owner->transform.worldPosition.y;

    if (drawAnchor) {
        debug.DrawLine(owner->transform.worldPosition, anchor, { 0.2f, 1.0f, 0.4f, 1.0f });
        debug.DrawSphere(anchor, 0.06f, { 0.2f, 1.0f, 0.4f, 1.0f });
    }

    // UI 要素の localScale.xy は倍率ではなく Canvas ピクセル単位の幅・高さ。
    // 追従帯は本体と同じ矩形。差が出るのは塗り潰し量だけ。
    const Vector3 barOrigin{ border, border, 0.0f };
    const Vector3 barSize{ width - border * 2.0f, height - border * 2.0f, 1.0f };

    backgroundObject->transform.position = Vector3::ZERO;
    backgroundObject->transform.scale    = { width, height, 1.0f };
    drainObject->transform.position      = barOrigin;
    drainObject->transform.scale         = barSize;
    fillObject->transform.position       = barOrigin;
    fillObject->transform.scale          = barSize;
}

inline float EnemyHealthBarComponent::AdvanceDrain(float ratio)
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    if (ratio >= m_drainRatio) {
        // 回復と初期化。遅らせる理由がないので即座に合わせる。
        m_drainRatio    = ratio;
        m_holdRemaining = 0.0f;
    } else {
        // 新しく減った瞬間だけ保持時間を入れ直す。連続被弾でも毎回「溜め」が入る。
        if (ratio < m_lastRatio) m_holdRemaining = std::max(drainHold, 0.0f);
        if (m_holdRemaining > 0.0f)
            m_holdRemaining = std::max(0.0f, m_holdRemaining - dt);
        else
            m_drainRatio = std::max(ratio, m_drainRatio - std::max(drainSpeed, 0.05f) * dt);
    }

    m_lastRatio = ratio;
    return m_drainRatio;
}

inline void EnemyHealthBarComponent::OnLateUpdate()
{
    const auto* health       = scene.GetScript<EnemyHealthComponent>();
    GameObject* canvasObject = m_canvas.Resolve(scene);
    if (!health || !canvasObject) return;

    const float ratio = health->Normalized();
    // 追従帯は隠れている間も進める。止めると、再表示した瞬間に古い削れ量が残って見える。
    const float drainRatio = AdvanceDrain(ratio);

    const bool visible = health->IsAlive() && !(hideWhenFull && ratio >= 1.0f);
    if (auto* canvas = canvasObject->GetComponent<UICanvas>())
        canvas->enabled = visible;
    if (!visible) return;

    Layout();

    GameObject* drainObject = m_drain.Resolve(scene);
    GameObject* fillObject  = m_fill.Resolve(scene);

    ui.SetImageColor(m_background.Resolve(scene), backgroundColor);
    ui.SetImageFillAmount(drainObject, drainRatio);
    ui.SetImageColor(drainObject, drainColor);
    ui.SetImageFillAmount(fillObject, ratio);
    ui.SetImageColor(fillObject, emptyColor + (fullColor - emptyColor) * ratio);
}

inline void EnemyHealthBarComponent::OnDestroy()
{
    // ルートに置いた以上、敵と一緒には消えない。持ち主が畳む。
    if (GameObject* canvasObject = m_canvas.Resolve(scene))
        scene.Destroy(*canvasObject);
    m_canvas = m_background = m_drain = m_fill = EntityRef{};
}

} // namespace sandbox
