// FBZZ Engine
// PlayerWorldSpaceUIComponent.hpp | sandbox
// WorldSpace UI テスト用: 指定した名前の GO 頭上にネームプレートを表示するスクリプト
//
// WHY: このスクリプト自体は Player とは別の空 GameObject に追加して使う。
//      エンジンが 1 GO につき 1 ScriptComponent の設計のため、
//      Player に直接乗せると PlayerControllerComponent と競合する。
//      OnStart で Scene::Find(targetName) で追跡対象を探し、
//      OnLateUpdate で毎フレーム headOffset 分だけ上の位置へ Canvas を追従させる。
#pragma once

#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace sandbox {

struct PlayerWorldSpaceUIComponent : fbzz::scene::Script {
    static constexpr const char* TYPE_NAME = "PlayerWorldSpaceUIComponent";

    const char* GetTypeName() const override { return TYPE_NAME; }

    // Inspector / Serializer からフィールドを公開する
    // targetName: 追跡する GO の名前 (Scene::Find で検索)
    std::string targetName  = "Player";
    std::string displayName = "Player";
    float       headOffset  = 2.2f;  // 追跡 GO の position から頭上までのオフセット (world 単位)

    void Reflect(fbzz::scene::IReflector& r) override
    {
        r.Field("Target Name",  targetName);
        r.Field("Display Name", displayName);
        r.Field("Head Offset",  headOffset);
    }

    void OnStart() override
    {
        if (!m_scene) return;

        // 追跡先 GO を名前で検索する。見つからなければ m_gameObject 自身を使う。
        m_targetGO = m_scene->Find(targetName);
        if (!m_targetGO) m_targetGO = m_gameObject;

        // ----------------------------------------------------------------
        // キャンバス Root GO を生成 (Scene の root に置く)
        // WHY: CollectCanvases() はルート GO のみ UICanvas を探索するため、
        //      親を持たない root GameObject として配置する必要がある。
        // ----------------------------------------------------------------
        auto& canvasGO = m_scene->CreateGameObject("__NamePlate_Canvas__");
        m_canvasID = canvasGO.GetID();

        fbzz::scene::UICanvas canvas;
        canvas.renderMode   = fbzz::scene::UIRenderMode::WorldSpace;
        canvas.canvasWidth  = 300.0f;   // キャンバスの幅 (px)
        canvas.canvasHeight = 60.0f;    // キャンバスの高さ (px)
        // 1 px = 0.003 world unit → パネルの実サイズ ≈ 0.9m × 0.18m
        canvas.worldScale   = 0.003f;
        canvas.sortOrder    = 10;
        canvasGO.AddComponent(canvas);

        // ----------------------------------------------------------------
        // 背景バー: 半透明の黒い矩形
        // transform.localPosition.xy = キャンバスローカルの左上座標 (px)
        // transform.localScale.xy    = 幅・高さ (px)
        // ----------------------------------------------------------------
        auto& bgGO = m_scene->CreateGameObject("__NamePlate_BG__");
        bgGO.SetParent(canvasGO);
        bgGO.transform.localPosition = { 0.0f, 0.0f, 0.0f };
        bgGO.transform.localScale    = { 300.0f, 60.0f, 1.0f };

        fbzz::scene::UIImage bg;
        bg.color = { 0.0f, 0.0f, 0.0f, 0.65f };  // 半透明黒
        bgGO.AddComponent(bg);

        // ----------------------------------------------------------------
        // テキストラベル: プレイヤー名を表示する
        // ----------------------------------------------------------------
        auto& textGO = m_scene->CreateGameObject("__NamePlate_Text__");
        textGO.SetParent(canvasGO);
        // キャンバス内 (20px, 14px) から描画開始
        textGO.transform.localPosition = { 20.0f, 14.0f, 0.0f };

        fbzz::scene::UIText label;
        label.text     = displayName;
        label.fontSize = 32.0f;
        label.color    = { 1.0f, 1.0f, 1.0f, 1.0f };
        textGO.AddComponent(label);

        SyncPosition();
    }

    // PhysicsSystem + TransformSystem の実行後なので追跡対象の position が最新になっている
    void OnLateUpdate(float) override { SyncPosition(); }

    void OnDestroy() override
    {
        if (m_scene) {
            if (auto* go = m_scene->GetGameObject(m_canvasID))
                fbzz::scene::GameObject::Destroy(*go);
        }
    }

private:
    fbzz::scene::EntityID  m_canvasID = fbzz::scene::EntityID::INVALID;
    fbzz::scene::GameObject* m_targetGO = nullptr;  // 非所有参照

    void SyncPosition()
    {
        if (!m_scene || !m_targetGO) return;
        auto* canvasGO = m_scene->GetGameObject(m_canvasID);
        if (!canvasGO) return;

        const fbzz::math::Vector3 headPos =
            m_targetGO->transform.position
            + fbzz::math::Vector3{ 0.0f, headOffset, 0.0f };

        // 親なし root GO なので localPosition = world position
        canvasGO->transform.localPosition = headPos;
        canvasGO->transform.position      = headPos;
        // 回転なし: 常にワールド軸正面を向く (UISystem が VP 行列で正しく投影する)
    }
};

} // namespace sandbox
