// FBZZ Engine
// SceneManager.hpp | fbzz::scene
// シーン遷移とアクティブ Scene 管理
// LoadScene 要求を保持し、フレーム境界で安全に切り替える。
// Scene の所有は manager が持ち、利用側は非所有参照で扱う。
#pragma once
#include "Scene.hpp"
#include <Physics/World.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class SceneManager {
public:
    using SceneFactory = std::function<std::unique_ptr<Scene>()>;

    // シーンファクトリを登録する
    void Register(const std::string& name, SceneFactory factory);

    // .fbzz ファイルからシーンを登録する。ロードは LoadScene 呼び出し時に行う
    void RegisterFromFile(const std::string& name, const std::string& path,
                          renderer::ResourceManager& resources);

    // 次フレームの先頭でシーンを切り替える
    void LoadScene(const std::string& name);

    // Physics + Transform + FlushDestroyQueue を実行する。
    // RenderSystem はゲームループ側から BeginFrame/EndFrame の間に直接呼ぶこと
    void Update(float dt, physics::World& world);

    Scene* GetActive();

private:
    std::unordered_map<std::string, SceneFactory> m_factories;
    std::unique_ptr<Scene>                        m_active;
    // LoadScene() が呼ばれた時点では切り替えず、次フレームの Update() 先頭で適用する。
    // フレーム途中に m_active を差し替えると、走査中の System が dangling 参照を掴む危険がある。
    std::string                                   m_pendingLoad;
};

} // namespace fbzz::scene
