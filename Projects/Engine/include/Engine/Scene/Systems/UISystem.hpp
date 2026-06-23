// FBZZ Engine
// UISystem.hpp | fbzz::scene
// ランタイム UI の描画とボタン入力処理
// UICanvas / UIImage / UIText / UIButton を走査して DrawCall とヒット状態を作る。
// WorldSpace UI には viewProjection を渡して座標変換する。
#pragma once
#include <Engine/Renderer/FontAtlas.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Matrix4.hpp>
#include <string>
#include <unordered_map>

namespace fbzz {
namespace renderer {
class IRenderer;
class ResourceManager;
}
namespace scene {
class Scene;
}
}

namespace fbzz::scene {

enum class UIRenderTargetView {
    GameViewport,  // 実ゲーム出力。ScreenSpace / WorldSpace の両方を最終合成として描く。
    SceneViewport, // 3D 編集ビュー。シーン内オブジェクトである WorldSpace Canvas だけ描く。
    CanvasEditor   // UI 専用編集ビュー。3D 空間に属する WorldSpace Canvas は描かない。
};

// Viewport / Scene ごとに一つ生成してライフタイムを呼び出し元が管理する。
// WHY: 複数 Viewport（Game / Scene / CanvasEditor）が同一フレームに UISystem を呼ぶとき、
//      static リソースを共有すると定数バッファが上書きし合う。インスタンスで分離する。
struct UISystemContext {
    bool initialized = false;
    std::string defaultFontPath =
        "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght";

    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::ShaderTag>         textShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> constants;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    renderer::ResourceHandle<renderer::PipelineStateTag>  worldPso;
    renderer::ResourceHandle<renderer::TextureTag>        whiteTexture;
    renderer::ResourceHandle<renderer::BufferTag>         imageVB;
    renderer::ResourceHandle<renderer::BufferTag>         textVB;
    std::unordered_map<std::string, renderer::FontAtlas>  fontAtlasCache;
};

// UIText.fontPath が空のときに使用するデフォルトフォントアトラスのベースパス (拡張子なし) を設定する。
void UISystemSetDefaultFontPath(UISystemContext& ctx, const std::string& basePath);

// フォントアトラスキャッシュをクリアする。シーン破棄・アセットリロード時に呼ぶ。
void UISystemFlushCache(UISystemContext& ctx);

// cameraWorldPos / cameraWorldRot:
//   WorldSpace ヒット判定と ScreenSpaceCamera モードのためのカメラ姿勢。
//   WorldSpace / ScreenSpaceCamera を使わない場合はデフォルト値でよい。
// viewProjection: WorldSpace / ScreenSpaceCamera canvas 用のカメラ VP 行列。
// targetView: Game / UI Editor など、呼び出し元 Viewport の役割に応じて描画対象 Canvas を絞る。
void UISystem(Scene& scene,
              renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              UISystemContext& ctx,
              float viewportWidth,
              float viewportHeight,
              math::Vector2 mouseInViewport,
              bool mousePressed,
              math::Vector3    cameraWorldPos = math::Vector3::ZERO,
              math::Quaternion cameraWorldRot = math::Quaternion::Identity(),
              const math::Matrix4& viewProjection = math::Matrix4::Identity(),
              UIRenderTargetView targetView = UIRenderTargetView::GameViewport);

} // namespace fbzz::scene
