// FBZZ Engine
// SceneManager.hpp | fbzz::scene
// シーン遷移とアクティブ Scene 管理
// LoadScene 要求を保持し、フレーム境界で安全に切り替える。
// Scene の所有は manager が持ち、利用側は非所有参照で扱う。
#pragma once
#include "Scene.hpp"
#include "Engine/Core/Scheduler/SystemScheduler.hpp"
#include <Physics/World.hpp>

namespace fbzz::audio { class AudioManager; }
#include <functional>
#include <memory>
#include <string_view>
#include <string>
#include <unordered_map>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class SceneManager {
public:
    SceneManager();

    using SceneFactory = std::function<std::unique_ptr<Scene>()>;

    void Register(const std::string& name, SceneFactory factory);

    // .fbzz ファイルからシーンを登録する。ロードは LoadScene 呼び出し時に行う
    void RegisterFromFile(const std::string& name, const std::string& path,
                          renderer::ResourceManager& resources);

    // 次フレームの先頭でシーンを切り替える。
    // 未登録の名前なら要求を受け付けず false を返す (エラーはログへ)。
    // WHY 戻り値を返すか: 遷移はフェードアウトの後に呼ばれることが多く、失敗を
    //     黙って捨てると「暗転したまま何も起きない」という最も気付きにくい形で止まる。
    bool LoadScene(const std::string& name);

    // 外部所有シーンをバインドする。null を渡すと LoadScene で作成した m_active を使用する
    void SetScene(Scene* scene);

    // 外部所有SceneとManager所有Sceneを両方破棄し、遷移要求も取り消す。
    // WHY: Script DLLをFreeLibraryする前に、どちらのSceneに残る派生Scriptも破棄する必要がある。
    void ClearScenes();

    // 固定タイムステップの Hz (デフォルト 60)
    void SetPhysicsHz(int hz);

    // false のとき EditorOnly System のみ実行する (エディタ停止中)
    void SetSimulating(bool simulating);

    // AudioSystem に渡す AudioManager を設定する（null を渡すと AudioSystem はスキップ）
    void SetAudioManager(audio::AudioManager* audioManager);

    // true のとき accumulator を無視して physics を 1 回だけ実行する (frame step 用)
    void SetSingleStep(bool singleStep);

    // Phase PreScript 〜 Cleanup
    // RenderSystem はゲームループ側から BeginFrame/EndFrame の間に直接呼ぶこと
    void Update(float dt, physics::World& world);

    // Phase LateUpdate のみ
    void LateUpdate(float dt, physics::World& world);

    Scene* GetActive();

    // 名前で System インスタンスを取得（エディタ統合用）。
    [[nodiscard]] ISystem* FindSystem(std::string_view name) const {
        return m_scheduler.FindSystem(name);
    }

private:
    Scene* CurrentScene() const;
    void BuildScheduler();

    std::unordered_map<std::string, SceneFactory> m_factories;
    std::unique_ptr<Scene>                        m_active;
    std::string                                   m_pendingLoad;
    Scene*                                        m_externalScene  = nullptr;
    bool                                          m_simulating     = true;
    audio::AudioManager*                          m_audioManager   = nullptr;

    SystemScheduler m_scheduler;
};

} // namespace fbzz::scene
