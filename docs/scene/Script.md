# Script / Reflect 設計書

`fbzz::scene::Script` — ユーザー定義のゲームロジックを GameObject に付与するスクリプト基盤。  
`IReflector` Visitor パターンにより、フィールドの宣言 1 箇所で Inspector 表示・TOML シリアライズ両方を賄う。

---

## 設計方針

| 観点 | 方針 |
|------|------|
| Component との関係 | `ScriptComponent` が `Script*` を所有する。既存 ECS (plain struct + System) と完全共存 |
| ロジックの所在 | Script はロジックを持つ唯一の例外。Engine のアーキテクチャ原則 "Component はデータのみ" はプリミティブ Component に適用する |
| フィールド宣言 | `Reflect(IReflector&)` に 1 度書けば Inspector 表示・保存・読み込みが自動 |
| dynamic_cast 回避 | `Script::GetTypeName()` + `static constexpr TYPE_NAME` でダウンキャストなしに型判定 |
| 1 GameObject = 1 Script | `ComponentArray` の制約上、1 Entity に ScriptComponent は 1 つ。複数挙動はプリミティブ Component で補う |

---

## 依存関係

```
engine/Scene/Script.hpp
├── math/Vector3.hpp
├── math/Vector4.hpp
├── math/Quaternion.hpp
└── <string>

editor/ImGuiReflector.hpp
├── engine/Scene/Script.hpp   (IReflector)
└── imgui.h

engine/Scene/ScriptComponent.hpp
└── engine/Scene/Script.hpp   (Script)

engine/Scene/Systems/ScriptSystem.hpp
├── engine/Scene/Scene.hpp
└── engine/Scene/ScriptComponent.hpp
```

---

## IReflector

```cpp
// engine/include/engine/Scene/Script.hpp  (IReflector + Script を同居)
namespace fbzz::scene {

struct IReflector {
    virtual ~IReflector() = default;

    virtual void Field(const char* name, float&           v) = 0;
    virtual void Field(const char* name, int&             v) = 0;
    virtual void Field(const char* name, bool&            v) = 0;
    virtual void Field(const char* name, math::Vector3&   v) = 0;
    virtual void Field(const char* name, math::Vector4&   v) = 0;
    virtual void Field(const char* name, std::string&     v) = 0;
    virtual void Field(const char* name, math::Quaternion& v) = 0;
};

} // namespace fbzz::scene
```

> **拡張メモ**: 将来型を増やす場合は `IReflector` に仮想関数を追加し、  
> `ImGuiReflector` / `TomlReflector` の両方に実装する。

---

## Script 基底クラス

```cpp
namespace fbzz::scene {

class Script {
public:
    virtual ~Script() = default;

    virtual void OnStart()           {}   // 初回 Update 前に 1 度だけ呼ばれる
    virtual void OnUpdate(float dt)  {}   // 毎フレーム呼ばれる
    virtual void Reflect(IReflector& r) {} // Inspector / Serializer から呼ばれる

    // dynamic_cast 禁止のため型名で判定する
    virtual const char* GetTypeName() const { return "Script"; }

    bool enabled = true;

protected:
    // ScriptSystem が設定する。ゲームコードは Read-Only で使う
    class Scene*      m_scene      = nullptr;  // 非所有参照
    class GameObject* m_gameObject = nullptr;  // 非所有参照

    friend class ScriptSystem; // 実際は ScriptSystem free function が設定
};

} // namespace fbzz::scene
```

> `m_scene` / `m_gameObject` は `ScriptSystem` が `OnStart` 前に設定する。  
> Unity の `this.gameObject` / `FindObjectOfType<T>()` 相当の操作が可能になる。

---

## ScriptComponent

```cpp
// engine/include/engine/Scene/ScriptComponent.hpp
namespace fbzz::scene {

struct ScriptComponent {
    std::unique_ptr<Script> script;
    bool m_started = false;  // OnStart が発火済みか (ScriptSystem が管理)
};

} // namespace fbzz::scene
```

### GameObject ヘルパー (GameObject.hpp に追加)

```cpp
// AddScript<T> — make_unique + AddComponent をまとめた Sugar
template<typename T, typename... Args>
T& AddScript(Args&&... args);

// GetScript<T> — TYPE_NAME で型チェックしてダウンキャスト
template<typename T>
T* GetScript();
```

実装 (Scene.hpp 末尾 — 他の template 本体と同居):

```cpp
template<typename T, typename... Args>
T& GameObject::AddScript(Args&&... args) {
    auto& sc = AddComponent<ScriptComponent>();
    sc.script = std::make_unique<T>(std::forward<Args>(args)...);
    return static_cast<T&>(*sc.script);
}

template<typename T>
T* GameObject::GetScript() {
    auto* sc = GetComponent<ScriptComponent>();
    if (!sc || !sc->script) return nullptr;
    if (std::string_view(sc->script->GetTypeName()) != T::TYPE_NAME) return nullptr;
    return static_cast<T*>(sc->script.get());
}
```

---

## ScriptSystem

```cpp
// engine/include/engine/Scene/Systems/ScriptSystem.hpp
namespace fbzz::scene {
class Scene;
void ScriptSystem(Scene& scene, float dt);
}
```

```
ScriptSystem の処理順:
  for each ScriptComponent:
    if !m_started:
      sc.script->m_scene      = &scene     // 非所有参照をセット
      sc.script->m_gameObject = &go        // 同上
      sc.script->OnStart()
      sc.m_started = true
    if sc.script->enabled:
      sc.script->OnUpdate(dt)
```

`SceneManager::Update` での呼び出し順:

```
TransformSystem → PhysicsSystem → ScriptSystem → RenderSystem → AudioSystem
```

> ScriptSystem を PhysicsSystem の後に置く理由:  
> `OnUpdate` 内で物理結果 (位置・速度) を読んでから  
> Transform を書き換えるユーザーコードを想定している。

---

## ImGuiReflector (editor)

```cpp
// editor/include/editor/ImGuiReflector.hpp
namespace fbzz::editor {

struct ImGuiReflector : scene::IReflector {
    void Field(const char* name, float& v) override {
        ImGui::DragFloat(name, &v, 0.1f);
    }
    void Field(const char* name, int& v) override {
        ImGui::DragInt(name, &v);
    }
    void Field(const char* name, bool& v) override {
        ImGui::Checkbox(name, &v);
    }
    void Field(const char* name, math::Vector3& v) override {
        float arr[3] = { v.x, v.y, v.z };
        if (ImGui::DragFloat3(name, arr, 0.1f))
            v = { arr[0], arr[1], arr[2] };
    }
    void Field(const char* name, math::Vector4& v) override {
        float arr[4] = { v.x, v.y, v.z, v.w };
        if (ImGui::ColorEdit4(name, arr))
            v = { arr[0], arr[1], arr[2], arr[3] };
    }
    void Field(const char* name, std::string& v) override {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", v.c_str());
        if (ImGui::InputText(name, buf, sizeof(buf))) v = buf;
    }
    void Field(const char* name, math::Quaternion& v) override {
        // オイラー角 (度) で表示・編集し Quaternion に戻す
        // 実装は InspectorPanel の QuatToEulerDeg を再利用
    }
};

} // namespace fbzz::editor
```

---

## InspectorPanel 連携

`InspectorPanel::OnRender` の末尾に追加:

```cpp
// Script
if (auto* sc = go->GetComponent<scene::ScriptComponent>()) {
    if (sc->script) {
        const char* header = sc->script->GetTypeName();
        if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled", &sc->script->enabled);
            ImGui::Separator();
            editor::ImGuiReflector r;
            sc->script->Reflect(r);   // ← これだけで全フィールドが自動表示
        }
    }
}
```

---

## SceneSerializer 連携 (将来拡張)

v1 の SceneSerializer は `ScriptComponent` を対象外とする。  
将来的には以下の `TomlReflector` を engine 側に追加することで対応する。

```cpp
// engine/include/engine/Scene/TomlReflector.hpp (v2 以降)
namespace fbzz::scene {

struct TomlReflector : IReflector {
    toml::table& tbl;
    bool saving;

    void Field(const char* name, float& v) override {
        if (saving) tbl.insert(name, v);
        else if (auto* n = tbl[name].as_floating_point()) v = (float)n->get();
    }
    void Field(const char* name, bool& v) override {
        if (saving) tbl.insert(name, v);
        else if (auto* n = tbl[name].as_boolean()) v = n->get();
    }
    // ... (他型も同様)
};

} // namespace fbzz::scene
```

`SceneSerializer::Save` での呼び出しイメージ:

```toml
[gameobjects.Script]
typeName = "PlayerController"
speed    = 5.0
```

```cpp
// Save
auto scriptTbl = toml::table{};
scriptTbl.insert("typeName", sc->script->GetTypeName());
TomlReflector r{ scriptTbl, true };
sc->script->Reflect(r);

// Load
// ... typeName で Script サブクラスを Factory から生成し、TomlReflector で値を復元
```

> Load 時のサブクラス生成には ScriptFactory (型名 → make_unique) が別途必要。  
> これは v2 の設計時に詳細化する。

---

## ユーザースクリプト例

```cpp
// sandbox/src/Scripts/PlayerController.hpp
#pragma once
#include <engine/Scene/Script.hpp>

namespace sandbox {

struct PlayerController : fbzz::scene::Script {
    static constexpr const char* TYPE_NAME = "PlayerController";
    const char* GetTypeName() const override { return TYPE_NAME; }

    float speed     = 5.0f;
    float jumpForce = 8.0f;

    void Reflect(fbzz::scene::IReflector& r) override {
        r.Field("Speed",      speed);
        r.Field("Jump Force", jumpForce);
    }

    void OnStart() override {
        // m_gameObject->GetComponent<RigidBodyComponent>() 等が使える
    }

    void OnUpdate(float dt) override {
        // 入力読み取り・位置更新
    }
};

} // namespace sandbox
```

```cpp
// sandbox/src/main.cpp — アタッチ方法
auto& player = scene.CreateGameObject("Player");
player.AddScript<sandbox::PlayerController>();
// Inspector に "PlayerController" ヘッダーと Speed / Jump Force が自動表示される
```

---

## ファイル構成 (実装時)

```
engine/include/engine/Scene/
├── Script.hpp             IReflector + Script 基底クラス
├── ScriptComponent.hpp    ScriptComponent struct
└── Systems/
    └── ScriptSystem.hpp   free function 宣言

engine/src/Scene/Systems/
└── ScriptSystem.cpp

editor/include/editor/
└── ImGuiReflector.hpp     ImGui 実装 (header-only)

sandbox/src/Scripts/       ユーザースクリプト置き場 (例)
└── PlayerController.hpp
```

---

## Scene.hpp への変更

```cpp
// 追加 include
#include "ScriptComponent.hpp"

// Scene private に追加
ComponentArray<ScriptComponent> m_scriptComponents;

// SceneView::HasOne / GetRef / AddComponent / GetComponent / HasComponent /
// RemoveComponent / GetEntities に ScriptComponent ケースを追加
// (既存の LightComponent 追加パターンと同一)
```

---

## 実装順

| 順序 | タスク |
|------|--------|
| 1 | `Script.hpp` (IReflector + Script 基底) を作成 |
| 2 | `ScriptComponent.hpp` を作成 |
| 3 | `Scene.hpp` に ScriptComponent を登録 |
| 4 | `GameObject.hpp` に `AddScript<T>` / `GetScript<T>` を追加 |
| 5 | `ScriptSystem.hpp` / `.cpp` を作成 |
| 6 | `ImGuiReflector.hpp` を editor に作成 |
| 7 | `InspectorPanel.cpp` に Script セクションを追加 |
| 8 | sandbox にサンプルスクリプトを追加して動作確認 |
