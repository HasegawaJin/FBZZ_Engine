// FBZZ Engine
// Script.hpp | fbzz::scene
// User script base class and reflection visitor
#pragma once

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <string>

namespace fbzz::scene {

class GameObject;
class Scene;

struct IReflector {
    virtual ~IReflector() = default;

    virtual void Field(const char* name, float& v) = 0;
    virtual void Field(const char* name, int& v) = 0;
    virtual void Field(const char* name, bool& v) = 0;
    virtual void Field(const char* name, math::Vector3& v) = 0;
    virtual void Field(const char* name, math::Vector4& v) = 0;
    virtual void Field(const char* name, std::string& v) = 0;
    virtual void Field(const char* name, math::Quaternion& v) = 0;
};

class Script {
public:
    virtual ~Script() = default;

    virtual void OnStart() {}
    virtual void OnUpdate(float) {}
    virtual void Reflect(IReflector&) {}
    virtual const char* GetTypeName() const { return "Script"; }

    bool enabled = true;

    void SetContext(Scene* scene, GameObject* gameObject)
    {
        m_scene = scene;
        m_gameObject = gameObject;
    }

protected:
    Scene* m_scene = nullptr;
    GameObject* m_gameObject = nullptr;
};

} // namespace fbzz::scene
