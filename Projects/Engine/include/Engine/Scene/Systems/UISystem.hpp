// FBZZ Engine
// UISystem.hpp | fbzz::scene
// ランタイム UI の描画とボタン入力処理
// UICanvas / UIImage / UIText / UIButton を走査して DrawCall とヒット状態を作る。
// WorldSpace UI には viewProjection を渡して座標変換する。
#pragma once
#include <Engine/Renderer/FontAtlas.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <deque>
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
class GameObject;
}
}

namespace fbzz::scene {

enum class UIRenderTargetView {
    GameViewport,  // 実ゲーム出力。ScreenSpace / WorldSpace の両方を最終合成として描く。
    SceneViewport, // 3D 編集ビュー。シーン内オブジェクトである WorldSpace Canvas だけ描く。
    CanvasEditor   // UI 専用編集ビュー。3D 空間に属する WorldSpace Canvas は描かない。
};

// テキスト 1 行ぶんの分割結果。折り返しを入れると「どこで切れたか」を
// 測るときと描くときで共有しないと、行数と行幅が食い違う。
struct UITextLine {
    std::size_t begin = 0;   ///< text.text へのバイトオフセット (この行の先頭)
    std::size_t end   = 0;   ///< 同 (この行の終端。改行文字は含まない)
    float       width = 0.0f;
};

// UI の頂点。UISystem.cpp の UIVertex と同じレイアウトで、
// 作業領域を Context に置くためにヘッダーへ出したもの。
struct UIVertex2D {
    math::Vector2 pos;
    math::Vector2 uv;
};

// UIImage.materialPath から解決した 1 件ぶんの描画設定。
// WHY PSO まで持つか: .mat は blend_mode を持つので、加算合成のゲージと通常合成の
//     パネルが同じ Canvas に並ぶ。UI 既定の ALPHA_BLEND 固定 PSO を使い回すと
//     .mat に書いた blend_mode が黙って無視される。
// WHY 値で持つか (shared_ptr にしないか):
//   この構造体は materialCache (unordered_map) の要素としてしか存在しない。
//   unordered_map はノード単位で確保するので、rehash しても要素のアドレスは動かない
//   (無効化されるのはイテレータだけ)。所有権を共有する相手も居ないため、
//   shared_ptr は参照カウントと 2 回目の間接参照を足すだけになる。
struct UIMaterialBinding {
    renderer::Material                                   material;
    // 上書きを名前でバイトオフセットへ写像するのに要る。
    renderer::ShaderDescriptor                           descriptor;
    renderer::ResourceHandle<renderer::PipelineStateTag> screenPso;
    renderer::ResourceHandle<renderer::PipelineStateTag> worldPso;
    // 上書きを持つ要素だけが通す一時 cbuffer。
    // WHY マテリアルごとに 1 本で足りるか: 定数バッファの Update は Upload Arena の
    //     スライスを切るので、同じハンドルへ書いて Submit を繰り返しても、記録済みの
    //     Draw はそれぞれ自分の書き込み時点の中身を読む (頂点バッファはこれが効かない
    //     ため UISystemContext 側で本数を持っている)。
    renderer::ResourceHandle<renderer::ConstantBufferTag> overrideConstants;
    // 上書き合成の作業領域。cbuffer サイズはマテリアルごとに決まっているので、
    // 解決時に 1 度確保すれば以降のフレームで確保が起きない。
    // WHY FrameAllocator を使わないか: フレームアロケータは「毎フレーム捨てる」前提で
    //     毎回バンプするが、ここは同じ長さを使い回すだけなので、確保済みの領域を
    //     持っておくほうが安い (定常状態で確保回数 0)。
    std::vector<std::uint8_t>                            overrideScratch;
    bool valid = false;
};

// Viewport / Scene ごとに一つ生成してライフタイムを呼び出し元が管理する。
// WHY: 複数 Viewport（Game / Scene / CanvasEditor）が同一フレームに UISystem を呼ぶとき、
//      static リソースを共有すると定数バッファが上書きし合う。インスタンスで分離する。
struct UISystemContext {
    bool initialized = false;
    // 当たり判定に使っている矩形・アンカー・ピボットを重ねて描く。
    // 呼び出し側が RenderSettings::showUIRects から毎フレーム入れる。
    // WHY RenderSettings を直接見ないか: UISystem は Editor / Standalone / 各
    //     Viewport から呼ばれる自由関数で、設定の所有者を知らない。
    //     知っている側が渡すほうが、Viewport ごとに出し分けもできる。
    bool showRects = false;
    std::string defaultFontPath =
        "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght";

    // 今処理している Canvas の「Canvas 1px あたりの実画面ピクセル数」。
    // フォントを焼く解像度をこれで決める (fontSize はあくまで Canvas 空間の値なので、
    // これを掛けないと Editor の小さな Game ビューで字だけが潰れる)。
    //
    // WHY 引数ではなくここに置くか: 使うのはテキストの計測と描画の 2 箇所だけだが、
    //     そこへ届けるには Canvas から降りる再帰を全段引き回すことになる。
    //     Canvas に入るたびに書き、その Canvas を抜けるまで変わらない値。
    float textPixelScale = 1.0f;

    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::ShaderTag>         textShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> constants;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    renderer::ResourceHandle<renderer::PipelineStateTag>  worldPso;
    renderer::ResourceHandle<renderer::TextureTag>        whiteTexture;
    // キーは「フォントのパス + 焼いた解像度」。同じフォントでも表示サイズが違えば
    // 別実体になる (FontAtlas.hpp の WHY を参照)。
    std::unordered_map<std::string, renderer::FontAtlas>  fontAtlasCache;
    // .mat のパスで引く。1 フレームに何度も同じマテリアルが出てくるうえ、
    // 解決はシェーダーのロードとリフレクションを伴うので毎回やる値段ではない。
    // UISystemFlushCache がシーン破棄・アセットリロードで捨てる。
    std::unordered_map<std::string, UIMaterialBinding>    materialCache;

    // 子要素を sortOrder 順に並べる作業領域。UI 階層の深さでインデックスする。
    //
    // WHY 深さごとに持つか: 並べた結果は子の再帰処理が終わるまで読み続けるので、
    //     1 本を共有すると降りた先で親のリストが上書きされ、兄弟が消える。
    //
    // WHY Context に置くか: UI ノードごと・毎フレームに走る確保をここで殺す。
    //     深さは階層の段数ぶんしか無く、一度伸びれば以降の確保は 0 になる。
    //     Viewport ごとに Context が分かれているので、同一フレームに複数の
    //     Viewport が描いても互いの作業領域を踏まない。
    //
    // WHY vector ではなく deque か:
    //     再帰で 1 段深く降りるとき、その深さの領域を確保するために外側が伸びうる。
    //     vector だと伸ばした瞬間に全要素が移動し、呼び出し元が握っている
    //     「自分の深さのリスト」への参照が宙に浮く。deque は末尾を伸ばしても
    //     既存要素への参照が生き残るので、借りたまま降りられる。
    std::deque<std::vector<GameObject*>>                  childScratch;

    // テキスト 1 要素ぶんの行分割結果。測るときと描くときの両方が読む。
    // WHY 1 本で足りるか: 行分割は 1 つのテキストを処理し終えるまでしか生きず、
    //     その間に別のテキストへ入ることが無い (再帰の途中で使い回されない)。
    std::vector<UITextLine>                               textLineScratch;
    // グリフ頂点をアトラスのページごとに溜める領域。ページ数はフォント依存。
    std::vector<std::vector<UIVertex2D>>                  textPageScratch;
    // UILayoutGroup が並べ替えに使う子インデックス。
    std::vector<int>                                      layoutIndexScratch;

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
