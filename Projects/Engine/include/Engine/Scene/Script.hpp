// FBZZ Engine
// Script.hpp | fbzz::scene
// ユーザースクリプト基底クラスとリフレクション補助
// GameObject にアタッチされる振る舞いの共通ライフサイクルを定義する。
// engine 側の Component とは分け、ScriptComponent が所有する。
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <string>

namespace fbzz::scene {

struct ColliderComponent;
class GameObject;
class Scene;

struct CollisionInfo {
    GameObject* self = nullptr;
    GameObject* other = nullptr;
    const ColliderComponent* selfCollider = nullptr;
    const ColliderComponent* otherCollider = nullptr;
};

// Script::Reflect() に渡されるビジターインターフェース。
// Inspector が ImGui を介してフィールドを表示・編集し、
// SceneSerializer が JSON にシリアライズ/デシリアライズする際にこれを実装する。
struct IReflector {
    virtual ~IReflector() = default;

    virtual void Field(const char* name, float& v) = 0;
    virtual void Field(const char* name, int& v) = 0;
    virtual void Field(const char* name, bool& v) = 0;
    virtual void Field(const char* name, math::Vector2& v) = 0;
    virtual void Field(const char* name, math::Vector3& v) = 0;
    virtual void Field(const char* name, math::Vector4& v) = 0;
    virtual void Field(const char* name, std::string& v) = 0;
    virtual void Field(const char* name, math::Quaternion& v) = 0;
};

class Script {
public:
    virtual ~Script() = default;

    virtual void OnStart() {}                              // 初回 Update 直前に 1 度だけ呼ばれる
    virtual void OnUpdate(float) {}                        // 毎フレーム呼ばれる
    virtual void OnCollisionEnter(const CollisionInfo&) {}
    virtual void OnCollisionStay(const CollisionInfo&) {}
    virtual void OnCollisionExit(const CollisionInfo&) {}
    virtual void OnTriggerEnter(const CollisionInfo&) {}
    virtual void OnTriggerStay(const CollisionInfo&) {}
    virtual void OnTriggerExit(const CollisionInfo&) {}
    virtual void OnDestroy() {}                            // GameObject 破棄時に呼ばれる
    virtual void Reflect(IReflector&) {}                   // Inspector / Serializer からフィールドを列挙
    // GetScript<T>() の型判別に使う。派生クラスは TYPE_NAME static constexpr も定義する
    virtual const char* GetTypeName() const { return "Script"; }

    bool enabled = true;

    // ScriptSystem が各ライフサイクル呼び出しの前に設定する。
    // 派生クラスは m_scene / m_gameObject を介して Scene / GameObject にアクセスする。
    void SetContext(Scene* scene, GameObject* gameObject)
    {
        m_scene = scene;
        m_gameObject = gameObject;
    }

protected:
    Scene*      m_scene      = nullptr; // 非所有参照
    GameObject* m_gameObject = nullptr; // 非所有参照
};

} // namespace fbzz::scene
