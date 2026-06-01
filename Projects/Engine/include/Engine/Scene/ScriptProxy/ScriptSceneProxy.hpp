// FBZZ Engine
// ScriptSceneProxy.hpp | fbzz::scene
// Script から Scene / GameObject 操作へ転送するショートハンド
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>
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
    template<typename T> T* GetScript() const;
    template<typename T> T* GetScript(GameObject& go) const;
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
};

} // namespace fbzz::scene
