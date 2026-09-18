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
    GameViewport,  ///< 実ゲーム出力。ScreenSpace / WorldSpace の両方を最終合成として描く。
    SceneViewport, ///< 3D 編集ビュー。シーン内オブジェクトである WorldSpace Canvas だけ描く。
    CanvasEditor   ///< UI 専用編集ビュー。3D 空間に属する WorldSpace Canvas は描かない。
};

/// テキスト 1 行ぶんの分割結果。折り返しを入れると「どこで切れたか」を
/// 測るときと描くときで共有しないと、行数と行幅が食い違う。
struct UITextLine {
    std::size_t begin = 0;   ///< text.text へのバイトオフセット (この行の先頭)
    std::size_t end   = 0;   ///< 同 (この行の終端。改行文字は含まない)
    float       width = 0.0f;
    /// この行の送り高さ。リッチテキストで `<size>` を使うと行ごとに変わる。
    float       height = 0.0f;
    /// この行の末尾を "…" に置き換えて描く (TextOverflow::Ellipsis)。
    bool        ellipsis = false;
};

/// UI の頂点。UISystem.cpp の UIVertex と同じレイアウトで、
/// 作業領域を Context に置くためにヘッダーへ出したもの。
///
/// LAYOUT: Assets/Shaders/UI/UICommon.hlsli の UIVertexInput と
///         宣言順・詰め方を一致させること (入力レイアウトは VS のリフレクションから
///         宣言順に組み立てられるので、順番がずれると黙って別の値を読む)。
struct UIVertex2D {
    math::Vector2 pos;
    math::Vector2 uv;
    /// 頂点ごとの色。g_Color に掛かる。色を使わない描画では白を積む。
    math::Vector4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

/// UI パスの b0。UI は b0 を CameraConstants ではなく UIConstants として使う。
///
/// @note レイアウトは Assets/Shaders/UI/UICommon.hlsli の cbuffer UIConstants と一致させること。
///       ヘッダーに出す理由: エディタの UI マテリアルプレビューが本編と同じ矩形を焼くのに
///       同じレイアウトを要り、UISystem.cpp に閉じると写しが 2 つになる。
struct UIConstantsCB {
    math::Matrix4 ortho;  ///< Canvas 空間 → clip 空間
    math::Vector4 color;  ///< UIImage.color / UIText.color
    math::Vector4 uvRect; ///< xy = uvMin, zw = uvMax
    /// xy = この矩形の Canvas ピクセル寸法, zw = 予約。
    /// 角丸・枠線・影は「何ピクセルぶん」で決まる。UV だけだと縦横比で角の形が変わる。
    math::Vector4 rect;
};
static_assert(sizeof(UIConstantsCB) == 112, "UIConstantsCB layout mismatch with UICommon.hlsli");

/// クリップ領域を表す半平面 1 枚。dot(p - point, normal) >= 0 側を残す。
///
/// @note 半平面の並びで持つ理由: Mask は回転でき、Scroll View 内に Mask を置く入れ子も起きる。
///       回転した矩形どうしの共通部分は矩形にならないため「矩形 1 つ」では表せないが、半平面の
///       並びなら本数が増えるだけで手順は変わらない。
/// @note CPU で切る理由: シェーダー側で捨てる方式だと、クリップの呼び出しをマテリアル作者全員が
///       書き写す必要があり、書き忘れた 1 本だけがマスクを無視する。頂点段階で切れば全マテリアル
///       に等しく効く。
struct UIClipPlane {
    math::Vector2 point{};
    math::Vector2 normal{};
};

/// UIImage.materialPath から解決した 1 件ぶんの描画設定。
/// @note PSO まで持つ理由: .mat の blend_mode で加算合成のゲージと通常合成のパネルが同じ
///       Canvas に並ぶ。UI 既定の ALPHA_BLEND 固定 PSO を使い回すと blend_mode が無視される。
/// @note 値で持つ理由 (shared_ptr にしない): materialCache (unordered_map) の要素としてしか
///       存在せず、rehash しても要素アドレスは動かない (無効化はイテレータのみ)。所有権を
///       共有する相手も居ないため shared_ptr は参照カウント分だけ無駄になる。
struct UIMaterialBinding {
    renderer::Material                                   material;
    /// 上書きを名前でバイトオフセットへ写像するのに要る。
    renderer::ShaderDescriptor                           descriptor;
    renderer::ResourceHandle<renderer::PipelineStateTag> screenPso;
    renderer::ResourceHandle<renderer::PipelineStateTag> worldPso;
    /// 上書きを持つ要素だけが通す一時 cbuffer。
    /// @note マテリアルごとに 1 本で足りる: 定数バッファの Update は Upload Arena のスライスを
    ///       切るため、同じハンドルへ Submit を繰り返しても記録済み Draw はそれぞれの書き込み
    ///       時点を読む (頂点バッファはこれが効かないため本数を別途持つ)。
    renderer::ResourceHandle<renderer::ConstantBufferTag> overrideConstants;
    /// 上書き合成の作業領域。cbuffer サイズはマテリアルごとに決まるため、解決時に 1 度確保すれば
    /// 以降のフレームで確保が起きない。
    /// @note FrameAllocator を使わない理由: 毎フレーム捨てる前提でバンプするが、ここは同じ長さを
    ///       使い回すだけなので、確保済み領域を持つほうが安い (定常状態で確保回数 0)。
    std::vector<std::uint8_t>                            overrideScratch;
    bool valid = false;
};

/// Viewport / Scene ごとに一つ生成してライフタイムを呼び出し元が管理する。
/// @note 複数 Viewport (Game/Scene/CanvasEditor) が同一フレームに UISystem を呼ぶとき、
///       static リソースを共有すると定数バッファが上書きし合うため、インスタンスで分離する。
struct UISystemContext {
    bool initialized = false;
    /// 当たり判定に使っている矩形・アンカー・ピボットを重ねて描く。呼び出し側が
    /// RenderSettings::showUIRects から毎フレーム入れる。
    /// @note RenderSettings を直接見ない理由: UISystem は Editor/Standalone/各 Viewport から
    ///       呼ばれる自由関数で設定の所有者を知らない。知っている側が渡せば Viewport ごとの
    ///       出し分けもできる。
    bool showRects = false;
    std::string defaultFontPath =
        "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght";

    /// 今処理している Canvas の「Canvas 1px あたりの実画面ピクセル数」。フォントを焼く解像度を
    /// これで決める (fontSize は Canvas 空間の値なので、掛けないと小さな Game ビューで字が潰れる)。
    /// @note 引数でなくここに置く理由: 使うのはテキストの計測と描画の 2 箇所だが、そこへ届けるには
    ///       Canvas から降りる再帰を全段引き回すことになる。Canvas に入るたび書き、抜けるまで不変。
    float textPixelScale = 1.0f;

    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::ShaderTag>         textShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> constants;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    renderer::ResourceHandle<renderer::PipelineStateTag>  worldPso;
    /// 選択マスク用: 不透明・深度書き込みあり・カリング無し。
    /// @note 通常の UI PSO を使い回さない理由: UI は DEPTH_OFF のため選択マスク RT の深度が
    ///       初期値のままになり、SelectionOutline.hlsl の遮蔽判定が 3D の手前でだけ落ちて輪郭が消える。
    renderer::ResourceHandle<renderer::PipelineStateTag>  selectionMaskPso;
    renderer::ResourceHandle<renderer::TextureTag>        whiteTexture;
    /// キーは「フォントのパス + 焼いた解像度」。同じフォントでも表示サイズが違えば
    /// 別実体になる (理由は FontAtlas.hpp を参照)。
    std::unordered_map<std::string, renderer::FontAtlas>  fontAtlasCache;
    /// .mat のパスで引く。1 フレームに何度も同じマテリアルが出てくるうえ、
    /// 解決はシェーダーのロードとリフレクションを伴うので毎回やる値段ではない。
    std::unordered_map<std::string, UIMaterialBinding>    materialCache;

    /// 上の 2 つのキャッシュを «いつ捨てるか» の判定に使う版数。UISystem の入口で
    /// ResourceManager / AssetManager の現在値と突き合わせる。
    /// @note 呼び出し側に任せない理由: デバイスロストとアセット再取り込みのどちらも UI の
    ///       呼び出し側 (Editor/Runtime) が知らされる仕組みが無く、«忘れずに呼ぶ» を期待した
    ///       結果 .mat を編集しても再起動まで反映されない状態が長く残った。
    /// @note 初期値が 0 の理由: ResourceManager の版数は 1 始まり、AssetManager は 0 始まりで
    ///       «1 度も同期していない» を表せる。初回は空キャッシュを捨てるだけで無害。
    std::uint64_t cachedResetVersion    = 0;
    std::uint64_t cachedShaderVersion   = 0;
    int           cachedAssetGeneration = -1;

    /// 子要素を sortOrder 順に並べる作業領域。UI 階層の深さでインデックスする。
    /// @note 深さごとに持つ理由: 並べた結果は子の再帰処理が終わるまで読み続けるため、1 本を
    ///       共有すると降りた先で親のリストが上書きされ兄弟が消える。
    /// @note Context に置く理由: UI ノードごと・毎フレームの確保をここで殺す。深さは階層の
    ///       段数ぶんしか無く、Viewport ごとに Context が分かれているため互いの作業領域を踏まない。
    /// @note vector でなく deque の理由: 再帰で 1 段深く降りるとき外側が伸びうるが、vector だと
    ///       全要素移動で参照が宙に浮く。deque は末尾を伸ばしても既存要素の参照が生き残る。
    std::deque<std::vector<GameObject*>>                  childScratch;

    /// テキスト 1 要素ぶんの行分割結果。測るときと描くときの両方が読む。
    /// @note 1 本で足りる理由: 行分割は 1 つのテキストを処理し終えるまでしか生きず、その間に
    ///       別のテキストへ入ることが無い (再帰の途中で使い回されない)。
    std::vector<UITextLine>                               textLineScratch;
    /// グリフ頂点をアトラスのページごとに溜める領域。ページ数はフォント依存。
    std::vector<std::vector<UIVertex2D>>                  textPageScratch;
    /// UILayoutGroup が並べ替えに使う子インデックス。
    std::vector<int>                                      layoutIndexScratch;
    /// レイアウトが子のサイズを一度に見るための作業領域。
    /// 揃え・伸縮・Grid の折り返しは「全部の寸法が分かってから」でないと解けない。
    std::vector<math::Vector2>                            layoutSizeScratch;

    /// いま有効なクリップ半平面。Mask / Scroll View に入るたび 4 枚積み、抜けると外す。
    /// @note 深さごとに分けない理由: 積んだ面は子孫すべてに効き続けるため、1 本のスタックが
    ///       階層の状態をそのまま表す。
    std::vector<UIClipPlane>                              clipPlanes;

    /// DrawCall ごとに別の頂点バッファを配る (プールは使い回し、確保はウォームアップ中だけ)。
    /// @note DX12 は DrawCall をコマンドリストへ記録するだけで実行はフレーム末尾のため、1 本を
    ///       Draw のたびに上書きすると全 Draw が最後の頂点を読み、UI 要素が同じ矩形になる。
    ///       記録型を前提に DX11 も同じ経路へ揃える。前フレーム GPU が読んでいる実体も貸さない
    ///       (規則は DynamicBufferPool.hpp)。プールはフレームが変わったときだけ巻き戻すため、
    ///       1 フレームに同じ Context が複数回描いても記録済み Draw と実体の 1 対 1 は保たれる。
    /// @note 下限を 1 にする理由: 矩形 1 枚ごとに 1 本借りるため、既定 1024 頂点を下限にすると
    ///       本数×1024 頂点が常駐する。要求そのもの (2 の冪へ切り上げ) で足りる。
    renderer::DynamicVertexBufferPool vertexPool{ 1 };
};

/// UIText.fontPath が空のときに使用するデフォルトフォントアトラスのベースパス (拡張子なし) を設定する。
void UISystemSetDefaultFontPath(UISystemContext& ctx, const std::string& basePath);

/// Context が抱えている頂点バッファを ResourceManager へ返す。
///
/// UISystemContext は ResourceManager を知らないため、デストラクタでは返せない。
/// Context を捨てる側が明示的に呼ぶこと。Play セッションごとに作り直される
/// ProjectRuntime::m_gameUICtx がこれを呼ばないと、往復のたびに DrawCall 数ぶんの
/// 頂点バッファが取り残される。
void UISystemReleaseGpuResources(UISystemContext& ctx, renderer::ResourceManager& resources);

/// 直近の UI 入力処理で、ポインターがいずれかの UI 要素に吸われたか。
/// @note 無いとメニュー上のクリックがそのままゲーム側 (射撃・カメラ操作) にも届く。判定自体は
///       ProcessUIEventsRecursive が既に出しているが、外へ出す口が無かった。
/// @note GameViewport の入力パスだけが更新する。UI が 1 度も回っていなければ false。
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

/// cameraWorldPos / cameraWorldRot:
///   WorldSpace ヒット判定と ScreenSpaceCamera モードのためのカメラ姿勢。
///   WorldSpace / ScreenSpaceCamera を使わない場合はデフォルト値でよい。
/// viewProjection: WorldSpace / ScreenSpaceCamera canvas 用のカメラ VP 行列。
/// targetView: Game / UI Editor など、呼び出し元 Viewport の役割に応じて描画対象 Canvas を絞る。
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

/// isSelected が true を返した UI 要素の矩形を、いま束ねられている選択マスク RT へ白で塗る。
/// 呼び出し側が選択マスク RT をバインドしてから呼ぶこと。
/// @note 輪郭自体を UI パスで描かない理由: 3D オブジェクトの選択輪郭は SelectionOutline パスが
///       マスクの縁を検出して描く。UI だけ別に描くと太さも色も RenderSettings の
///       outlineColor/outlineWidth から外れ、同じ「選択中」が 2 種類の見た目になる。
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
