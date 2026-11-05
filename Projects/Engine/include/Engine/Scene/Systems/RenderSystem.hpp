/// @file    RenderSystem.hpp
/// @brief   Scene から DrawCall を生成する描画 System。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note MeshRenderer / Light / Camera / Transform を集約し、IRenderer へ送信する。
/// @note Renderer の具体実装には依存せず、ResourceManager とインターフェースだけを使う。
#pragma once
#include <Engine/Api.hpp>
#include <Graphics/Pipeline/RenderPassCapture.hpp>
#include <Physics/Layer.hpp>
#include <memory>
#include <span>
#include <string>
#include <Graphics/Effects/RenderInstanceInput.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/CameraCullingSettings.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>

namespace fbzz::scene    { class Scene; struct RenderFrameGeometryCache; struct ScriptDebugDrawCommand; }
namespace fbzz::physics  { class World; }
namespace fbzz::renderer {
    class IRenderer;
    class Camera;
    class ResourceManager;
    class SurfacePatchSelectionCacheOwner;
    struct RenderSettings;
}

namespace fbzz::scene {

using renderer::RenderPassCapture;

/// @note View-local authoring geometry; never added to Scene, serialization, physics or the shared geometry cache.
struct RenderInstancePreview {
    std::string definition;
    std::shared_ptr<const renderer::InstanceSetData> data;
    math::Matrix4 world = math::Matrix4::Identity();
    uint64_t identity = 0;
    bool erase = false;
    bool highlight = false;
};

/// @note RenderGraph 内へ UI 合成パスを登録するための設定。
/// @note UI を外で描くと PostProcess 後の RT バインド状態に依存し、複数 Viewport で別 RT へ
/// @note 描いてしまう。Output ReadWrite pass として扱い、3D/PostProcess/UI の順序をグラフ
/// @note 依存で固定する。
struct RenderSystemUIOptions {
    bool enabled = false;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    math::Vector2 mouseInCanvasSpace = {};
    bool mousePressed = false;
    UIRenderTargetView targetView = UIRenderTargetView::GameViewport;
    /// @note 呼び出し元 Viewport が所有する GPU リソースコンテキスト。null なら UI は描画されない。
    /// @note 複数 Viewport が同フレームに UISystem を呼ぶときインスタンスを分離し、定数バッファの
    /// @note 上書き競合を防ぐ。
    UISystemContext* context = nullptr;
    /// @note True for runtime Game View/standalone; false preserves raw Editor authoring preview input.
    bool gameplayInput = false;
    /// @note Borrowed only for this RenderSystem call; DebugDraw captures owned line/triangle values.
    /// @note Per-view diagnostics bypass the script flag and queue; at most 8192 commands are captured.
    std::span<const ScriptDebugDrawCommand> spatialDebugCommands;
    std::span<const RenderInstancePreview> instancePreviews;
};

FBZZ_ENGINE_API void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const renderer::Camera& camera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT = {},
                  const renderer::RenderSettings* settings = nullptr,
                  fbzz::LayerMask cullingMask = fbzz::Layer::Everything,
                  const RenderSystemUIOptions* uiOptions = nullptr,
                  const physics::World* physicsWorld = nullptr,
                  /// @note カリング挙動の上書き。nullptr のときはシーンのメインカメラから解決する。
                  /// @note Standalone/GameHub は RenderSystem を素朴に呼ぶだけなので既定は «シーンから解決»。
                  /// @note Editor の Scene View は自前のデバッグカメラで描くため、ゲームカメラの設定を
                  /// @note 持ち込まないよう明示的に既定値を渡す。
                  const CameraCullingSettings* cullingSettings = nullptr,
                  RenderPassCapture* capture = nullptr,
                  RenderFrameGeometryCache* frameGeometry = nullptr,
                  /// @note Optional caller-owned pure Water LOD cache; pinned for this call on the recording thread.
                  std::shared_ptr<renderer::SurfacePatchSelectionCacheOwner> surfacePatchPlans = {});

} /// @note namespace fbzz::scene
