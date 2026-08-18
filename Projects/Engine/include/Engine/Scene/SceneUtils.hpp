// FBZZ Engine
// SceneUtils.hpp | fbzz::scene
// 起動・実行時にシーン / 物理 / UI サブシステムへ ProjectSettings を適用するユーティリティ
//
// WHY: Sandbox と EditorLauncher の両方が同じ「設定 → サブシステム反映」処理を必要とする。
//      重複を避けるため Engine に集約し、どの起動モジュールからも参照できるようにする。
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

/// ProjectSettings の物理設定を physics::World へ反映する。
void ApplyPhysicsSettings(physics::World& world, const ProjectSettings& settings);

/// ProjectSettings の UI 設定を UI システムへ反映する。
/// ctx を渡すと defaultFontPath をそのコンテキストへ設定する。
/// 既存呼び出しは nullptr デフォルトのまま変更不要。
void ApplyUISettings(const ProjectSettings& settings, UISystemContext* ctx = nullptr);

/// ProjectSettings.window から Window::Config を生成する。
/// WHY: Sandbox / EditorLauncher の両方が Application::Init() 前に同じ変換を行うため共通化する。
[[nodiscard]] core::Window::Config MakeWindowConfig(const ProjectSettings& settings);

/// シーン内のメイン CameraComponent を renderer::Camera として解決する。
/// WHY: Standalone / Editor の双方が「isMain フラグを持つ最初の Camera GO」を使う規約を共通化する。
///      Main Camera が存在しない場合はアスペクト比だけ設定した既定カメラを返す。
[[nodiscard]] renderer::Camera ResolveGameCamera(Scene& scene, float aspectRatio);

/// エディタ向け: シーン内のメイン CameraComponent を editorFallback ベースで上書きして返す。
/// WHY: EditorModule と EditorLauncher の両方がデバッグカメラをフォールバックとする同じ解決ロジックを持っていたため共通化する。
[[nodiscard]] renderer::Camera ResolveEditorGameCamera(Scene& scene,
                                                       const renderer::Camera& editorFallback,
                                                       float aspectRatio);

/// シーン内のメイン CameraComponent の cullingMask を返す。見つからなければ Layer::Everything を返す。
[[nodiscard]] fbzz::LayerMask ResolveGameCullingMask(Scene& scene);

/// シーン内のメイン CameraComponent のカリング設定を返す。見つからなければ既定値を返す。
/// 実体は CameraCullingSettings.hpp (RenderSystem 側と共有する POD)。
[[nodiscard]] CameraCullingSettings ResolveGameCullingSettings(Scene& scene);

} // namespace fbzz::scene
