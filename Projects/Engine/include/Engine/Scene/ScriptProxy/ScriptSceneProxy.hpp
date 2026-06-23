// FBZZ Engine
// ScriptSceneProxy.hpp | fbzz::scene
// Script から Scene / GameObject 操作へ転送するショートハンド
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/PrefabRef.hpp>
#include <Math/Vector3.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

class GameObject;
class Script;

struct ScriptSceneProxy {
    Script* script = nullptr;

    GameObject* Find(std::string_view name) const;
    GameObject* FindWithTag(std::string_view tag) const;
    GameObject* Self() const;
    template<typename T> GameObject* FindObjectOfType() const;
    template<typename T> std::vector<GameObject*> FindObjectsOfType() const;
    GameObject* GetGameObject(EntityID id) const;
    GameObject* GetMainCameraObject() const;
    GameObject& Create(std::string_view name = "GameObject") const;
    void Destroy(GameObject& go, float delay = 0.0f) const;
    void DestroySelf(float delay = 0.0f) const;
    template<typename T> T* GetScript() const;
    template<typename T> T* GetScript(GameObject& go) const;
    template<typename T> T* GetScript(GameObject* go) const;  // nullptr 安全なポインタ版
    // EntityID から直接 Script を取得する。Inspector でアサインした参照に使う。
    template<typename T> T* GetScript(EntityID id) const;
    template<typename T> T* GetComponent() const;
    template<typename T> T& GetOrAddComponent() const;
    template<typename T> T& RequireComponent() const;
    bool IsActiveAndEnabled() const;
    bool isActiveAndEnabled() const { return IsActiveAndEnabled(); }
    void LoadScene(std::string_view name) const;
    std::string GetSceneName() const;
    float GetTerrainHeightAt(const math::Vector3& worldPos) const;
    math::Vector3 GetTerrainNormalAt(const math::Vector3& worldPos) const;
    float GetWaterSurfaceHeight(const math::Vector3& worldPos, float time) const;

    // 自 GO の name / tag ショートハンドプロパティ
    std::string GetName() const;
    void        SetName(const std::string& n) const;
    std::string GetTag() const;
    void        SetTag(const std::string& t) const;

    __declspec(property(get=GetName, put=SetName)) std::string name;
    __declspec(property(get=GetTag,  put=SetTag )) std::string tag;

    // Prefab をインスタンス化して最初のルート GO を返す。失敗時は nullptr。
    GameObject* Instantiate(const PrefabRef& prefab) const;
    GameObject* Instantiate(const std::string& prefabPath) const;
    // init コールバック付きオーバーロード — 生成直後の GO に初期値を差し込むのに使う。
    GameObject* Instantiate(const PrefabRef& prefab,
                            std::function<void(GameObject&)> init) const;
    GameObject* Instantiate(const std::string& prefabPath,
                            std::function<void(GameObject&)> init) const;
};

} // namespace fbzz::scene
