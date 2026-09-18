/// @file    SceneUtils.hpp
/// @brief   起動・実行時にシーン / 物理 / UI サブシステムへ ProjectSettings を適用するユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-03
///
/// @note Sandbox と EditorLauncher の両方が同じ「設定→サブシステム反映」処理を必要とするため、Engine に集約している。
#pragma once

#include <Engine/Core/Window.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/CameraCullingSettings.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Physics/Layer.hpp>
#include <Physics/World.hpp>

namespace fbzz::scene {
class Scene;
}

namespace fbzz::scene {

/// @brief ProjectSettings の物理設定を physics::World へ反映する。
void ApplyPhysicsSettings(physics::World& world, const ProjectSettings& settings);

/// @brief ProjectSettings の UI 設定を UI システムへ反映する。
/// @param ctx 渡すと defaultFontPath をそのコンテキストへ設定する。省略時 (nullptr) は設定しない。
void ApplyUISettings(const ProjectSettings& settings, UISystemContext* ctx = nullptr);

/// @brief ProjectSettings.window から Window::Config を生成する。
/// @note Sandbox/EditorLauncher の両方が Application::Init() 前に同じ変換を行うため共通化した。
[[nodiscard]] core::Window::Config MakeWindowConfig(const ProjectSettings& settings);

/// @brief シーン内のメイン CameraComponent を renderer::Camera として解決する。
/// @note Standalone/Editor の双方が「isMain フラグを持つ最初の Camera GO」を使う規約を共通化した。
/// @return Main Camera が存在しない場合はアスペクト比だけ設定した既定カメラ。
[[nodiscard]] renderer::Camera ResolveGameCamera(Scene& scene, float aspectRatio);

/// @brief エディタ向け: シーン内のメイン CameraComponent を editorFallback ベースで上書きして返す。
/// @note EditorModule と EditorLauncher の両方がデバッグカメラをフォールバックとする同じ解決ロジックを共通化した。
[[nodiscard]] renderer::Camera ResolveEditorGameCamera(Scene& scene,
                                                       const renderer::Camera& editorFallback,
                                                       float aspectRatio);

/// @brief シーン内のメイン CameraComponent の背景色を返す。見つからなければ既定色を返す。
/// @note Scene View はデバッグカメラの視点なのでカリング設定は持ち込まないが、背景色は «シーンがどう見えるか» そのものなので、この関数を通して本番と同じ色を出す。
[[nodiscard]] math::Vector4 ResolveGameBackgroundColor(Scene& scene);

/// @brief シーン内のメイン CameraComponent の cullingMask を返す。見つからなければ Layer::Everything を返す。
[[nodiscard]] fbzz::LayerMask ResolveGameCullingMask(Scene& scene);

/// @brief シーン内のメイン CameraComponent のカリング設定を返す。見つからなければ既定値を返す。
/// @note 実体は CameraCullingSettings.hpp で定義される POD。RenderSystem 側と共有する。
[[nodiscard]] CameraCullingSettings ResolveGameCullingSettings(Scene& scene);

} // namespace fbzz::scene
