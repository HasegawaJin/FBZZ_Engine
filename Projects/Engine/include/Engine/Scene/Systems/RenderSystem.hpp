/// @file    RenderSystem.hpp
/// @brief   Scene から DrawCall を生成する描画 System。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// MeshRenderer / Light / Camera / Transform を集約し、IRenderer へ送信する。
/// Renderer の具体実装には依存せず、ResourceManager とインターフェースだけを使う。
#pragma once
#include <Physics/Layer.hpp>
#include <memory>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/CameraCullingSettings.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>

namespace fbzz::scene    { class Scene; }
namespace fbzz::physics  { class World; }
namespace fbzz::renderer {
    class IRenderer;
    class Camera;
    class ResourceManager;
    struct RenderSettings;
}

namespace fbzz::scene {

class RenderPassCapture;

/// RenderGraph 内へ UI 合成パスを登録するための設定。
/// @note UI を外で描くと PostProcess 後の RT バインド状態に依存し、複数 Viewport で別 RT へ
///       描いてしまう。Output ReadWrite pass として扱い、3D/PostProcess/UI の順序をグラフ
///       依存で固定する。
struct RenderSystemUIOptions {
    bool enabled = false;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    math::Vector2 mouseInCanvasSpace = {};
    bool mousePressed = false;
    UIRenderTargetView targetView = UIRenderTargetView::GameViewport;
    /// 呼び出し元 Viewport が所有する GPU リソースコンテキスト。null なら UI は描画されない。
    /// @note 複数 Viewport が同フレームに UISystem を呼ぶときインスタンスを分離し、定数バッファの
    ///       上書き競合を防ぐ。
    UISystemContext* context = nullptr;
};

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const renderer::Camera& camera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT = {},
                  const renderer::RenderSettings* settings = nullptr,
                  fbzz::LayerMask cullingMask = fbzz::Layer::Everything,
                  const RenderSystemUIOptions* uiOptions = nullptr,
                  const physics::World* physicsWorld = nullptr,
                  /// カリング挙動の上書き。nullptr のときはシーンのメインカメラから解決する。
                  /// @note Standalone/GameHub は RenderSystem を素朴に呼ぶだけなので既定は «シーンから解決»。
                  ///       Editor の Scene View は自前のデバッグカメラで描くため、ゲームカメラの設定を
                  ///       持ち込まないよう明示的に既定値を渡す。
                  const CameraCullingSettings* cullingSettings = nullptr,
                  RenderPassCapture* capture = nullptr);

} // namespace fbzz::scene
