// FBZZ Engine
// SceneManager.hpp | fbzz::scene
// シーン遷移管理。LoadScene はフレーム末尾で適用する
#pragma once
#include "Scene.hpp"
#include <Physics/World.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace fbzz::renderer { class IRenderer; }

namespace fbzz::scene {

class SceneManager {
public:
    using SceneFactory = std::function<std::unique_ptr<Scene>()>;

    // シーンファクトリを登録する
    void Register(const std::string& name, SceneFactory factory);

    // .fbzz ファイルからシーンを登録する。ロードは LoadScene 呼び出し時に行う
    void RegisterFromFile(const std::string& name, const std::string& path,
                          renderer::IRenderer& renderer);

    // 次フレームの先頭でシーンを切り替える
    void LoadScene(const std::string& name);

    // Physics + Transform + FlushDestroyQueue を実行する。
    // RenderSystem はゲームループ側から BeginFrame/EndFrame の間に直接呼ぶこと
    void Update(float dt, physics::World& world);

    Scene* GetActive();

private:
    std::unordered_map<std::string, SceneFactory> m_factories;
    std::unique_ptr<Scene>                        m_active;
    std::string                                   m_pendingLoad;
};

} // namespace fbzz::scene
