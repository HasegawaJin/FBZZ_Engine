// FBZZ Engine
// StandaloneApp.hpp | fbzz::editor_launcher
// エディタ UI を持たないスタンドアロン (配布ゲーム) モードのライフサイクル管理。
//
// WHY: エディタと同じバイナリを --standalone フラグで起動するため、
//      EditorApp を使わずに ImGui を一切生成しないシンプルなゲームループを持つ。
//      EditorLauncher の内部ファイルであり fbzz_editor ライブラリに依存しない。
#pragma once
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>
#include <filesystem>
#include <memory>

namespace fbzz::renderer { class IRenderer; }

namespace fbzz::editor_launcher {

// スタンドアロンモードのゲームループを担うクラス。
// Init() でシーンをロードし、RunLoop() でゲームを実行する。
// EditorApp / ImGui に一切依存せず、エンジン・物理・シーンシステムのみを使う。
class StandaloneApp {
public:
    // シーンファイルをロードして物理ワールドを初期化する。
    // @param renderer     描画バックエンド
    // @param resources    GPU リソースマネージャ
    // @param assetRoot    Assets/ ディレクトリの絶対パス
    // @param sceneFile    ロードするシーンファイルの絶対パス
    // @param settings     ProjectSettings (ウィンドウ・物理設定を参照する)
    [[nodiscard]] bool Init(renderer::IRenderer& renderer,
                            renderer::ResourceManager& resources,
                            const std::filesystem::path& assetRoot,
                            const std::filesystem::path& sceneFile,
                            const ProjectSettings& settings);

    // メインゲームループ。Application::IsRunning() が false になるまで実行し続ける。
    void RunLoop(renderer::IRenderer& renderer,
                 renderer::ResourceManager& resources);

    // GPU リソースを解放する。
    void Shutdown();

private:
    // シーン内の isMain フラグを持つ CameraComponent を探してゲームカメラを返す。
    // WHY: Standalone モードでは EditorCamera が存在しないため、
    //      シーン内の CameraComponent を唯一のゲームカメラとして使う。
    //      見つからなければデフォルト値 (原点・正面向き) のカメラを返す。
    renderer::Camera ResolveGameCamera(float aspectRatio) const;

    std::unique_ptr<scene::Scene>   m_scene;
    std::unique_ptr<physics::World> m_physicsWorld;
    ProjectSettings                 m_settings;
    float                           m_physicsAccumulator = 0.0f;
};

} // namespace fbzz::editor_launcher
