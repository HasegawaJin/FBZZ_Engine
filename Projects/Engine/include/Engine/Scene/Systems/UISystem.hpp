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
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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
    std::unordered_map<std::string, renderer::FontAtlas>  fontAtlasCache;

    // WHY DrawCall ごとに別の頂点バッファを配るか:
    //   DX12 は DrawCall をコマンドリストへ *記録* するだけで、GPU が実行するのは
    //   フレーム末尾。1 本の頂点バッファを Draw のたびに上書きすると、実行時には
    //   記録済みの全 Draw が「最後に書かれた頂点」を読む = UI 要素が全部同じ矩形で
    //   描かれる。定数バッファは Update ごとに Upload Arena のスライスを切るため無事で、
    //   頂点バッファだけが共有実体のまま残っていた。
    //   DX11 は即時描画なので 1 本でも成立していたが、記録型を前提に両バックエンドを
    //   同じ経路へ揃える。バッファは使い回すので、確保はウォームアップ中だけ起きる。
    std::vector<renderer::ResourceHandle<renderer::BufferTag>> imageVertexBuffers;
    std::vector<renderer::ResourceHandle<renderer::BufferTag>> textVertexBuffers;
    std::size_t imageVertexCursor = 0;
    std::size_t textVertexCursor  = 0;
    // カーソルを戻すのはフレームが変わったときだけ。同じフレーム内で 1 つの Context が
    // 複数回描く場合 (Scene と Canvas Editor) も、記録済み Draw と実体の 1 対 1 を保つ。
    std::uint64_t lastResetFrame = ~std::uint64_t{ 0 };
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
