// FBZZ Engine
// SceneManager.hpp | fbzz::scene
// シーン遷移管理。LoadScene はフレーム末尾で適用する
#pragma once
#include "Scene.hpp"
#include <engine/Renderer/IRenderer.hpp>
#include <physics/World.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace fbzz::scene {

class SceneManager {
public:
    using SceneFactory = std::function<std::unique_ptr<Scene>()>;

    // シーンファクトリを登録する
    void Register(const std::string& name, SceneFactory factory);

    // 次フレームの先頭でシーンを切り替える
    void LoadScene(const std::string& name);

    // System を順に呼ぶ。ゲームループから毎フレーム呼ぶ
    void Update(float dt,
                renderer::IRenderer& renderer,
                physics::World& world);

    Scene* GetActive();

private:
    std::unordered_map<std::string, SceneFactory> m_factories;
    std::unique_ptr<Scene>                        m_active;
    std::string                                   m_pendingLoad;
};

} // namespace fbzz::scene
