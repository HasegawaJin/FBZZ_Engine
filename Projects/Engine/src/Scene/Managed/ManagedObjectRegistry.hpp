/// @file    ManagedObjectRegistry.hpp
/// @brief   Scene と Entity の正確な寿命を単調 object token へ接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#pragma once
#include <Engine/Api.hpp>
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
    FBZZ_ENGINE_API ManagedObjectRegistry();
    FBZZ_ENGINE_API ~ManagedObjectRegistry();
    ManagedObjectRegistry(const ManagedObjectRegistry&) = delete;
    ManagedObjectRegistry& operator=(const ManagedObjectRegistry&) = delete;
    FBZZ_ENGINE_API uint64_t Register(Scene& scene, GameObject& object);
    FBZZ_ENGINE_API GameObject* Resolve(Scene& scene, uint64_t token) const;
    FBZZ_ENGINE_API const Record* Find(uint64_t token) const;
    FBZZ_ENGINE_API void RetireObject(Scene& scene, EntityID entity);
    FBZZ_ENGINE_API void RetireScene(Scene& scene);
    /// @note token sequence は reset しない。CLR が保持する旧 token と再起動後の token を区別する。
    FBZZ_ENGINE_API void Clear();
    FBZZ_ENGINE_API size_t Count() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
