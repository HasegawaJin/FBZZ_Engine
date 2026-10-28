/// @file    ManagedObjectRegistry.hpp
/// @brief   Scene と Entity の正確な寿命を単調 object token へ接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <cstddef>
#include <memory>
#include <string>

namespace fbzz::scene {
class Scene;
class GameObject;
namespace managed_detail {
/// @note メインスレッド限定。Scene の破棄通知は pointer 比較だけで記録を回収し、managed callback を呼ばない。
class ManagedObjectRegistry final {
public:
    struct Record { Scene* scene; uint64_t sceneGeneration; EntityID entity; std::string guid; };
    ManagedObjectRegistry();
    ~ManagedObjectRegistry();
    ManagedObjectRegistry(const ManagedObjectRegistry&) = delete;
    ManagedObjectRegistry& operator=(const ManagedObjectRegistry&) = delete;
    uint64_t Register(Scene& scene, GameObject& object);
    GameObject* Resolve(Scene& scene, uint64_t token) const;
    const Record* Find(uint64_t token) const;
    void RetireObject(Scene& scene, EntityID entity);
    void RetireScene(Scene& scene);
    /// @note token sequence は reset しない。CLR が保持する旧 token と再起動後の token を区別する。
    void Clear();
    size_t Count() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
