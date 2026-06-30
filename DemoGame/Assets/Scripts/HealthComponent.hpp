// FBZZ Engine
// HealthComponent.hpp | sandbox
// Player / Enemy 共通の体力 (HP) スクリプト。
//
// 設計意図 (WHY):
//   - 体力データ・ダメージ処理・ゲージ UI・死亡演出を 1 つに集約し、Player と Enemy で
//     同じスクリプトを使い回す。表示形態だけ barMode で切り替える:
//       ScreenHUD       … 画面左上に固定する HUD ゲージ (Player 用)。
//       WorldBillboard  … キャラ頭上に出し常にカメラへ正対する 3D ゲージ (Enemy 用)。
//   - UI は OnStart で動的生成する。シーンに UI を手置きしなくても、動的スポーンした敵に
//     そのままゲージが付く。生成した UIImage の fillAmount を ScriptUIProxy 経由で毎フレーム
//     更新するだけで残量を表現する (engine 側に追加した塗り潰し機能を利用)。
//   - ダメージは AttackHitboxComponent が命中時に TakeDamage() を呼んで与える。
//     体力が 0 になったら Death トリガーを発火し、コントローラー側が IsDead() を見て停止する。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Scene.hpp>        // GameObject::AddComponent / GetScript の実体
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include "GameVocab.hpp"
// 標準ヘッダー (<algorithm>/<cmath>/<string>/<vector>) は Script.hpp が共通プレリュードとして提供する。

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// 体力ゲージの表示形態。
enum class HealthBarMode {
    ScreenHUD      = 0, // 画面固定 HUD (Player)
    WorldBillboard = 1, // ワールド空間・カメラ正対 (Enemy)
};

class HealthComponent : public Script {
    FBZZ_SCRIPT(HealthComponent)

public:
    FBZZ_GROUP("Health")
    FBZZ_FIELD_RANGE(float, maxHealth, 100.0f, "Max Health", 1.0f, 10000.0f)
    FBZZ_FIELD_ENUM(HealthBarMode, barMode, HealthBarMode::ScreenHUD, "Bar Mode",
                    "Screen HUD", "World Billboard")

    FBZZ_GROUP("Screen HUD Layout")
    // 画面左上を原点 (y 下向き) としたピクセル座標とサイズ (1920x1080 基準)。
    FBZZ_FIELD(Vector2, hudPosition, Vector2(48.0f, 42.0f), "HUD Position")
    FBZZ_FIELD(Vector2, hudSize,     Vector2(460.0f, 30.0f), "HUD Size")
    FBZZ_FIELD(std::string, hudLabel, "PLAYER", "HUD Label")

    FBZZ_GROUP("World Billboard Layout")
    FBZZ_FIELD_RANGE(float, worldHeight,  2.25f, "World Height",   0.0f, 6.0f)
    FBZZ_FIELD(Vector2, worldBarSize, Vector2(120.0f, 16.0f), "World Bar Size")
    FBZZ_FIELD_RANGE(float, worldScale, 0.01f, "World Scale", 0.001f, 0.05f)

    FBZZ_GROUP("Colors")
    FBZZ_FIELD(Vector4, colorFull,       Vector4(0.20f, 0.82f, 0.25f, 1.0f), "Color Full")
    FBZZ_FIELD(Vector4, colorLow,        Vector4(0.88f, 0.16f, 0.16f, 1.0f), "Color Low")
    FBZZ_FIELD(Vector4, colorBackground, Vector4(0.05f, 0.05f, 0.07f, 0.85f), "Color Background")

    FBZZ_GROUP("Animator")
    FBZZ_FIELD(std::string, paramDeath, AnimParam::Death, "Death Trigger Param")

    FBZZ_GROUP("Death")
    // true のとき、Death モーション後にディゾルブしてオブジェクトを削除する (Enemy 用)。
    // Player のように死亡しても残すキャラは false にする。
    FBZZ_FIELD(bool, destroyOnDeath,        false, "Destroy On Death")
    // 既定 false。Player のように死亡後も地面に立たせるキャラはコライダーを残す。
    // 削除して落下させたくない場合は destroyOnDeath と併用し静的化する Enemy で true にする。
    FBZZ_FIELD(bool, removeColliderOnDeath, false, "Remove Collider On Death")
    FBZZ_FIELD(std::string, dissolveMaterial,
               "Assets/Materials/Skinned/SkinnedDissolve_Enemy.mat", "Dissolve Material")
    FBZZ_FIELD(std::string, dissolveParam, "alphaCutoff", "Dissolve Param")
    // 死亡してからディゾルブ開始までの猶予 (Death モーションを見せる時間)。
    FBZZ_FIELD_RANGE(float, dissolveDelay,    1.8f, "Dissolve Delay",    0.0f, 10.0f)
    FBZZ_FIELD_RANGE(float, dissolveDuration, 1.6f, "Dissolve Duration", 0.1f, 10.0f)

    // ── ランタイム API (他スクリプトから呼ぶ) ────────────────────────────────
    void TakeDamage(float amount);
    void Heal(float amount);
    // 最大 HP を設定し満タンで初期化する (共有ステータスアセット等からの初期化に使う)。
    // WHY: maxHealth(public) を書き換えるだけだと、HealthComponent::OnStart の方が後に走った場合は
    //      m_current = maxHealth で上書きされて整合するが、先に走った場合は現在値が旧 max のまま残る。
    //      ここで maxHealth と m_current を両方更新することで OnStart の実行順に依存せず正しく反映する。
    void InitHealth(float newMaxHealth) {
        maxHealth = std::max(1.0f, newMaxHealth);
        m_current = maxHealth;
        RefreshBar(); // UI 未生成 (m_fill==nullptr) の間は内部でスキップされる
    }
    [[nodiscard]] bool  IsDead() const { return m_dead; }
    [[nodiscard]] float HealthRatio() const {
        return maxHealth > 0.0f ? std::clamp(m_current / maxHealth, 0.0f, 1.0f) : 0.0f;
    }

    void OnStart() override;
    void OnUpdate() override;

private:
    void BuildUI();
    void RefreshBar();
    void Die();
    void TickDeathSequence(); // 死亡後の Dissolve → 削除を進める
    void StartDissolve();     // 全メッシュ子をディゾルブマテリアルへ差し替える
    void CollectMeshObjects();// MaterialComponent を持つ子孫 GO を集める
    // 共通: 親 GO 下に UIImage 1 枚を持つ子 GO を作る。pos/size はキャンバスピクセル座標。
    GameObject* CreateImage(GameObject& parent, const std::string& name,
                            const Vector2& pos, const Vector2& size,
                            const Vector4& color, int sortOrder,
                            bool asFill);

    float m_current = 0.0f;
    bool  m_dead    = false;
    bool  m_built   = false;
    float m_deathElapsed   = 0.0f;
    bool  m_dissolveStarted = false;
    GameObject* m_canvas = nullptr; // ゲージのルート Canvas GO
    GameObject* m_fill   = nullptr; // fillAmount を毎フレーム更新する UIImage GO
    GameObject* m_text   = nullptr; // HUD の数値テキスト (WorldBillboard では nullptr)
    std::vector<GameObject*> m_meshObjects; // ディゾルブ対象のメッシュ子 GO
};

FBZZ_REFLECT(HealthComponent)

inline void HealthComponent::OnStart()
{
    m_current = maxHealth;
    BuildUI();
    RefreshBar();
}

inline void HealthComponent::OnUpdate()
{
    if (m_dead) {
        // 死亡後はゲージ更新を止め、Dissolve → 削除のシーケンスだけを進める。
        if (destroyOnDeath)
            TickDeathSequence();
        return;
    }
    // ビルボードは UISystem 側の faceCamera が向きを処理するため、ここでは残量表示のみ更新する。
    RefreshBar();
}

inline void HealthComponent::TakeDamage(float amount)
{
    if (m_dead || amount <= 0.0f) return;
    m_current = std::max(0.0f, m_current - amount);
    RefreshBar();
    if (m_current <= 0.0f)
        Die();
}

inline void HealthComponent::Heal(float amount)
{
    if (m_dead || amount <= 0.0f) return;
    m_current = std::min(maxHealth, m_current + amount);
    RefreshBar();
}

inline void HealthComponent::Die()
{
    if (m_dead) return;
    m_dead = true;

    // Death モーションへ遷移し、以後の移動・攻撃はコントローラーが IsDead() で停止する。
    animator.SetTrigger(paramDeath);

    // WHY: 入力を切っても直前の速度で滑り続けるため、死亡時点で水平速度を止める。
    if (physics.HasRigidBody()) {
        Vector3 v = physics.GetVelocity();
        v.x = 0.0f;
        v.z = 0.0f;
        physics.SetVelocity(v);
    }

    // 死亡した時点で当たり判定を外し、死体が移動を阻んだり再被弾したりしないようにする。
    if (removeColliderOnDeath)
        collider.SetEnabled(false);

    if (destroyOnDeath) {
        // WHY: コライダーを外すと重力で落下し続けるため、静的化して死体をその場に留める。
        physics.SetStatic(true);
        // 残量ゲージは即座に隠す (空バーが Dissolve 中に浮かんで見えないように)。
        if (m_canvas && m_canvas->IsValid())
            m_canvas->SetActive(false);
    }
}

inline void HealthComponent::TickDeathSequence()
{
    m_deathElapsed += Time::deltaTime;
    if (m_deathElapsed < dissolveDelay) return; // Death モーションを見せる猶予

    if (!m_dissolveStarted) {
        StartDissolve();
        m_dissolveStarted = true;
    }

    // alphaCutoff を 0→1 へ上げると SkinnedDissolve.hlsl が clip でメッシュを溶かす。
    const float t = dissolveDuration > 0.0f
        ? (m_deathElapsed - dissolveDelay) / dissolveDuration
        : 1.0f;
    const float cutoff = std::clamp(t, 0.0f, 1.0f);
    for (GameObject* go : m_meshObjects)
        material.SetFloat(go, dissolveParam, cutoff);

    // 完全に溶けきったらオブジェクトごと削除する。
    if (cutoff >= 1.0f)
        scene.Destroy(*m_gameObject);
}

inline void HealthComponent::StartDissolve()
{
    CollectMeshObjects();
    for (GameObject* go : m_meshObjects) {
        material.SetMaterial(go, dissolveMaterial);
        material.SetFloat(go, dissolveParam, 0.0f);
    }
}

inline void HealthComponent::CollectMeshObjects()
{
    m_meshObjects.clear();
    if (!m_gameObject) return;

    // MaterialComponent を持つ子孫 GO (スキンドメッシュのサブメッシュ群) を集める。
    // 反復スタックで階層全体を走査する。
    std::vector<GameObject*> stack;
    for (int i = 0, n = m_gameObject->GetChildCount(); i < n; ++i)
        if (GameObject* c = m_gameObject->GetChild(i)) stack.push_back(c);

    while (!stack.empty()) {
        GameObject* go = stack.back();
        stack.pop_back();
        if (!go) continue;
        if (go->GetComponent<MaterialComponent>())
            m_meshObjects.push_back(go);
        for (int i = 0, n = go->GetChildCount(); i < n; ++i)
            if (GameObject* c = go->GetChild(i)) stack.push_back(c);
    }
}

inline GameObject* HealthComponent::CreateImage(GameObject& parent, const std::string& name,
                                                const Vector2& pos, const Vector2& size,
                                                const Vector4& color, int sortOrder,
                                                bool asFill)
{
    GameObject& go = scene.Create(name);
    go.SetParent(parent);
    // UI の transform: position.xy = キャンバスピクセル座標、scale.xy = ピクセルサイズ。
    go.transform.position = Vector3(pos.x, pos.y, 0.0f);
    go.transform.scale    = Vector3(size.x, size.y, 1.0f);

    UIImage image{};
    image.color     = color;
    image.sortOrder = sortOrder;
    if (asFill) {
        // 体力バーは左端を固定し右へ減る。残量は fillAmount で表現する。
        image.fillOrigin = UIImageFillOrigin::Left;
        image.fillAmount = 1.0f;
    }
    go.AddComponent<UIImage>(image);
    return &go;
}

inline void HealthComponent::BuildUI()
{
    if (m_built) return;
    m_built = true;

    constexpr float PAD = 4.0f; // 背景の内側余白 (枠線風に見せる)

    if (barMode == HealthBarMode::ScreenHUD) {
        // 画面固定の HUD キャンバス (解像度追従)。
        GameObject& canvas = scene.Create("HealthHUD_Canvas");
        UICanvas uc{};
        uc.renderMode   = UIRenderMode::ScreenSpaceOverlay;
        uc.scaleMode    = UICanvasScaleMode::ScaleWithScreenSize;
        uc.canvasWidth  = 1920.0f;
        uc.canvasHeight = 1080.0f;
        uc.referenceWidth  = 1920.0f;
        uc.referenceHeight = 1080.0f;
        uc.sortOrder    = 50;
        canvas.AddComponent<UICanvas>(uc);
        m_canvas = &canvas;

        CreateImage(canvas, "HealthHUD_BG", hudPosition, hudSize, colorBackground, 0, false);
        m_fill = CreateImage(canvas, "HealthHUD_Fill",
                             hudPosition + Vector2(PAD, PAD),
                             hudSize - Vector2(PAD * 2.0f, PAD * 2.0f),
                             colorFull, 1, true);

        // ラベル + 数値テキスト (バーの上)。
        GameObject& textGO = scene.Create("HealthHUD_Text");
        textGO.SetParent(canvas);
        textGO.transform.position = Vector3(hudPosition.x, hudPosition.y - 30.0f, 0.0f);
        UIText txt{};
        txt.text      = hudLabel;
        txt.fontSize  = 26.0f;
        txt.color     = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        txt.sortOrder = 2;
        textGO.AddComponent<UIText>(txt);
        m_text = &textGO;
        return;
    }

    // WorldBillboard: キャラ頭上に出すワールド空間ゲージ。常にカメラへ正対させる。
    GameObject& canvas = scene.Create("Health_Billboard");
    canvas.SetParent(*m_gameObject);
    canvas.transform.position = Vector3(0.0f, worldHeight, 0.0f); // 親 (キャラ) からの相対
    UICanvas uc{};
    uc.renderMode   = UIRenderMode::WorldSpace;
    uc.faceCamera   = true; // engine 側に追加したビルボード機能
    uc.canvasWidth  = worldBarSize.x;
    uc.canvasHeight = worldBarSize.y;
    uc.worldScale   = worldScale;
    uc.sortOrder    = 10;
    canvas.AddComponent<UICanvas>(uc);
    m_canvas = &canvas;

    CreateImage(canvas, "Health_Billboard_BG",
                Vector2(0.0f, 0.0f), worldBarSize, colorBackground, 0, false);
    m_fill = CreateImage(canvas, "Health_Billboard_Fill",
                         Vector2(PAD, PAD),
                         worldBarSize - Vector2(PAD * 2.0f, PAD * 2.0f),
                         colorFull, 1, true);
}

inline void HealthComponent::RefreshBar()
{
    const float ratio = HealthRatio();

    // 残量バーの長さ (fillAmount) と色 (緑→赤の線形補間) を更新する。
    if (m_fill && m_fill->IsValid()) {
        ui.SetImageFillAmount(m_fill, ratio);
        const Vector4 c = colorLow + (colorFull - colorLow) * ratio;
        ui.SetImageColor(m_fill, c);
    }

    // HUD の数値テキスト (例: "PLAYER  73/100")。
    if (m_text && m_text->IsValid()) {
        const int cur = static_cast<int>(std::ceil(m_current));
        const int max = static_cast<int>(maxHealth);
        ui.SetText(m_text, hudLabel + "  " + std::to_string(cur) + "/" + std::to_string(max));
    }
}

} // namespace sandbox
