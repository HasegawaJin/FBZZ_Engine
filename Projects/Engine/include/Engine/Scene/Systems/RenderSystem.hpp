// FBZZ Engine
// RenderSystem.hpp | fbzz::scene
// Scene から DrawCall を生成する描画 System
// MeshRenderer / Light / Camera / Transform を集約し、IRenderer へ送信する。
// Renderer の具体実装には依存せず、ResourceManager とインターフェースだけを使う。
#pragma once
#include <Physics/Layer.hpp>
#include <memory>
#include <Engine/Renderer/ResourceHandle.hpp>
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

// RenderSystemUIOptions は RenderGraph 内へ UI 合成パスを登録するための設定。
// WHY: UI を RenderSystem の外で描くと、PostProcess 後の RT バインド状態に依存して
//      Editor の複数 Viewport で別 RT へ描いてしまうリスクがある。
//      RenderGraph の Output ReadWrite pass として扱えば、3D / PostProcess / UI の順序を
//      グラフ依存で固定できる。
struct RenderSystemUIOptions {
    bool enabled = false;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    math::Vector2 mouseInCanvasSpace = {};
    bool mousePressed = false;
    UIRenderTargetView targetView = UIRenderTargetView::GameViewport;
    // 呼び出し元 Viewport が所有する GPU リソースコンテキスト。
    // WHY: 複数 Viewport が同フレームに UISystem を呼ぶとき、インスタンスを分離して
    //      定数バッファの上書き競合を防ぐ。null の場合 UI は描画されない。
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
                  const physics::World* physicsWorld = nullptr);

} // namespace fbzz::scene
