/// @file    UISystem.hpp
/// @brief   ランタイム UI の描画とボタン入力処理。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// UICanvas / UIImage / UIText / UIButton を走査して DrawCall とヒット状態を作る。
/// WorldSpace UI には viewProjection を渡して座標変換する。
#pragma once
#include <Engine/Renderer/DynamicBufferPool.hpp>
#include <Engine/Renderer/FontAtlas.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <deque>
#include <functional>
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

struct UICanvas;

/// 描画と同じ規則で求める Canvas 全体の寸法。safeArea を差し引く前の Canvas 単位。
[[nodiscard]] math::Vector2 GetCanvasRectSize(const UICanvas& canvas,
    float viewportWidth, float viewportHeight);

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
    /// この行の送り高さ。リッチテキストで <size> を使うと行ごとに変わる。
    float       height = 0.0f;
    /// この行の末尾を "…" に置き換えて描く (TextOverflow::Ellipsis)。
    bool        ellipsis = false;
};

// UI の頂点。UISystem.cpp の UIVertex と同じレイアウトで、
// 作業領域を Context に置くためにヘッダーへ出したもの。
//
// LAYOUT: Assets/Shaders/UI/UICommon.hlsli の UIVertexInput と
//         宣言順・詰め方を一致させること (入力レイアウトは VS のリフレクションから
//         宣言順に組み立てられるので、順番がずれると黙って別の値を読む)。
struct UIVertex2D {
    math::Vector2 pos;
    math::Vector2 uv;
    // 頂点ごとの色。g_Color に掛かる。色を使わない描画では白を積む。
    math::Vector4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

// クリップ領域を表す半平面 1 枚。dot(p - point, normal) >= 0 側を残す。
//
// WHY 矩形ではなく半平面の並びで持つか:
//   Mask は回転できるうえ、Scroll View の中に Mask を置くような入れ子も起きる。
//   回転した矩形どうしの共通部分は矩形にならないので、「矩形 1 つ」では表せない。
//   半平面の並びなら、入れ子は単に本数が増えるだけで、切り取りの手順は変わらない。
// WHY GPU ではなく CPU で切るか:
//   シェーダーで捨てる方式にすると、UI マテリアルを書く人全員が
//   クリップの呼び出しを書き写す必要があり、書き忘れた 1 本だけが
//   マスクを無視して描かれる。頂点の段階で切れば、どのマテリアルにも等しく効く。
struct UIClipPlane {
    math::Vector2 point{};
    math::Vector2 normal{};
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
    // 選択マスク用: 不透明・深度書き込みあり・カリング無し。
    // WHY 通常の UI PSO を使い回さないか: UI は DEPTH_OFF なので選択マスク RT の
    //     深度が初期値 (最遠) のままになり、SelectionOutline.hlsl の遮蔽判定
    //     (選択物の深度 <= シーン深度) が 3D の手前でだけ落ちて輪郭が消える。
    renderer::ResourceHandle<renderer::PipelineStateTag>  selectionMaskPso;
    renderer::ResourceHandle<renderer::TextureTag>        whiteTexture;
    // キーは「フォントのパス + 焼いた解像度」。同じフォントでも表示サイズが違えば
    // 別実体になる (FontAtlas.hpp の WHY を参照)。
    std::unordered_map<std::string, renderer::FontAtlas>  fontAtlasCache;
    // .mat のパスで引く。1 フレームに何度も同じマテリアルが出てくるうえ、
    // 解決はシェーダーのロードとリフレクションを伴うので毎回やる値段ではない。
    std::unordered_map<std::string, UIMaterialBinding>    materialCache;

    // 上の 2 つのキャッシュを «いつ捨てるか» の判定に使う版数。
    // UISystem の入口で ResourceManager / AssetManager の現在値と突き合わせる。
    //
    // WHY 呼び出し側に任せないか: 捨てる必要があるのはデバイスロストとアセット再取り込みの
    //     2 つだが、どちらも UI の呼び出し側 (Editor / Runtime) が知らされる仕組みが無い。
    //     «忘れずに呼ぶ» を期待した結果、UI の .mat を編集しても再起動まで反映されない
    //     状態が長く残った。版数を持っている側と突き合わせて自分で捨てる。
    // WHY 初期値が 0 か: ResourceManager の版数は 1 始まり、AssetManager は 0 始まりで
    //     «1 度も同期していない» を表せる。初回は空のキャッシュを捨てるだけで無害。
    std::uint64_t cachedResetVersion    = 0;
    std::uint64_t cachedShaderVersion   = 0;
    int           cachedAssetGeneration = -1;

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
    // レイアウトが子のサイズを一度に見るための作業領域。
    // 揃え・伸縮・Grid の折り返しは「全部の寸法が分かってから」でないと解けない。
    std::vector<math::Vector2>                            layoutSizeScratch;

    // いま有効なクリップ半平面。Mask / Scroll View に入るたび 4 枚積み、抜けると外す。
    // WHY 深さごとに分けないか: 積んだ面は子孫すべてに効き続けるので、
    //     1 本のスタックが階層の状態をそのまま表す。
    std::vector<UIClipPlane>                              clipPlanes;

    // WHY DrawCall ごとに別の頂点バッファを配るか:
    //   DX12 は DrawCall をコマンドリストへ *記録* するだけで、GPU が実行するのは
    //   フレーム末尾。1 本の頂点バッファを Draw のたびに上書きすると、実行時には
    //   記録済みの全 Draw が「最後に書かれた頂点」を読む = UI 要素が全部同じ矩形で
    //   描かれる。定数バッファは Update ごとに Upload Arena のスライスを切るため無事で、
    //   頂点バッファだけが共有実体のまま残っていた。
    //   DX11 は即時描画なので 1 本でも成立していたが、記録型を前提に両バックエンドを
    //   同じ経路へ揃える。バッファは使い回すので、確保はウォームアップ中だけ起きる。
    //   前フレームの GPU がまだ読んでいる実体も貸さない (規則は DynamicBufferPool.hpp)。
    //   1 フレームに同じ Context が複数回描く場合 (Scene と Canvas Editor、選択マスクと本描画) も、
    //   プールはフレームが変わったときだけ巻き戻すので、記録済み Draw と実体の 1 対 1 が保たれる。
    // WHY 下限を 1 にするか: 矩形 1 枚ごとに 1 本借りるので、既定の 1024 頂点を下限にすると
    //     本数 × 1024 頂点が常駐する。要求そのもの (2 の冪へ切り上げ) で足りる。
    renderer::DynamicVertexBufferPool vertexPool{ 1 };
};

// UIText.fontPath が空のときに使用するデフォルトフォントアトラスのベースパス (拡張子なし) を設定する。
void UISystemSetDefaultFontPath(UISystemContext& ctx, const std::string& basePath);

/// Context が抱えている頂点バッファを ResourceManager へ返す。
///
/// UISystemContext は ResourceManager を知らないため、デストラクタでは返せない。
/// Context を捨てる側が明示的に呼ぶこと。Play セッションごとに作り直される
/// ProjectRuntime::m_gameUICtx がこれを呼ばないと、往復のたびに DrawCall 数ぶんの
/// 頂点バッファが取り残される。
void UISystemReleaseGpuResources(UISystemContext& ctx, renderer::ResourceManager& resources);

/// 直近の UI 入力処理で、ポインターがいずれかの UI 要素に吸われたか。
///
/// WHY 要るか: これが無いと、メニューの上でクリックした入力がそのまま
///     ゲーム側 (射撃・カメラ操作) にも届く。判定そのものは
///     ProcessUIEventsRecursive が既に出しているのに、外へ出す口が無かった。
/// NOTE: GameViewport の入力パスだけが更新する。値はフレーム単位で、
///       UI が 1 度も回っていなければ false。
[[nodiscard]] bool UIPointerOverUI();

/// リッチテキストのタグを除いた文字数。visibleCharacters の上限を知るのに使う。
/// richText が false なら単純な UTF-8 の文字数。
[[nodiscard]] std::size_t UITextVisibleLength(const std::string& text, bool richText);

/// フォントアトラスとマテリアル解決のキャッシュを捨てる。
///
/// 通常は UISystem が入口で版数を見て自分で呼ぶ (UISystemContext の版数フィールドを参照)。
/// 明示的に呼ぶのは «版数では表せない» 捨て方をしたいときだけ。
///
/// @param resources 抱えている GPU リソースの返却先。デバイスリセット後は nullptr を渡すこと
///                  (実体はもう無く、ハンドルだけが残っている)。
void UISystemFlushCache(UISystemContext& ctx, renderer::ResourceManager* resources);

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

// isSelected が true を返した UI 要素の矩形を、いま束ねられている選択マスク RT へ
// 白で塗る。呼び出し側が選択マスク RT をバインドしてから呼ぶこと。
//
// WHY 輪郭そのものを UI パスで描かないか: 3D オブジェクトの選択輪郭は
//     SelectionOutline パスがマスクの縁を検出して描いている。UI だけ別に描くと
//     太さも色も RenderSettings の outlineColor / outlineWidth から外れ、
//     同じ「選択中」が 2 種類の見た目になる。塗るのはマスクまでにする。
void UISelectionMaskSystem(Scene& scene,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           UISystemContext& ctx,
                           float viewportWidth,
                           float viewportHeight,
                           const std::function<bool(GameObject&)>& isSelected,
                           math::Vector3    cameraWorldPos = math::Vector3::ZERO,
                           math::Quaternion cameraWorldRot = math::Quaternion::Identity(),
                           const math::Matrix4& viewProjection = math::Matrix4::Identity(),
                           UIRenderTargetView targetView = UIRenderTargetView::GameViewport);

} // namespace fbzz::scene
