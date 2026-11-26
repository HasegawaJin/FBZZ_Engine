/// @file    RenderPassCapture.hpp
/// @brief   選択パス直後の画像と実行情報、およびリソース一覧をビュー単位で保持する。
/// @author  Hasegawa Jin
/// @date    2026-09-15

/// @note 2 つの見方を 1 つのキャプチャが受け持つ。
/// @note Preview … 選んだ 1 パスの直後。パスの «途中経過» を見る
/// @note Gallery … フレーム末尾の全リソース。いま何が差さっているかを «一望» する
/// @note どちらも同じ診断シェーダーで焼くので、表示モードの意味も同じになる。
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/RenderGraph/RenderGraph.hpp>
#include <Graphics/Renderer/Diagnostics/GpuProfiler.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>

#include <string>
#include <vector>

namespace fbzz::renderer {

struct RenderPassContext;

class RenderPassCapture {
public:
    RenderPassCapture() = default;
    RenderPassCapture(const RenderPassCapture&) = delete;
    RenderPassCapture& operator=(const RenderPassCapture&) = delete;

    struct PassInfo {
        std::string name; ///< グラフに登録されたパス名。同名のパスは occurrence で区別する。
        size_t occurrence = 0; ///< 同名パスの中での出現順。
        size_t graphIndex = 0; ///< 元の RenderGraph でこのパスを指す添字。
        bool culled = false; ///< このフレームの実行計画から除外されたか。
        double cpuMs = -1.0; ///< CPU で計測した所要時間（ミリ秒）。負値は計測値なし。
        double gpuMs = -1.0; ///< GPU で計測した所要時間（ミリ秒）。負値は計測値なし。
        std::vector<renderer::RenderGraph::ResourceAccess> accesses; ///< パスが申告した論理リソースとそのアクセス方法。
    };

    struct OutputInfo {
        std::string name; ///< ビューアーでキャプチャ対象として選ぶ出力名。
        renderer::ResourceHandle<renderer::TextureTag> texture; ///< 選択した出力の画像を参照するテクスチャ。
        bool depth = false; ///< 深度画像として表示する出力か。
        bool cameraDepth = false; ///< カメラの投影条件で線形化できる深度か。
        uint32_t width = 0; ///< 元の画像の幅（画素）。
        uint32_t height = 0; ///< 元の画像の高さ（画素）。
    };

    enum class DisplayMode { AUTO, RGB, RED, GREEN, BLUE, ALPHA, SIGNED_VECTOR, LINEAR_DEPTH };

    /// @note 画像 1 枚の «見せ方»。Preview と Gallery が同じものを使う。
    struct ViewSettings {
        DisplayMode mode = DisplayMode::AUTO; ///< RGB、単一チャンネル、深度などの可視化方式。
        float exposure = 0.0f; ///< プレビュー表示の露出補正（段）。1 段で明るさを 2 倍にする。
        float rangeMin = 0.0f; ///< プレビュー上で表示範囲の下端に対応させる入力値。
        float rangeMax = 1.0f; ///< プレビュー上で表示範囲の上端に対応させる入力値。
        bool  gamma = false; ///< 表示時にガンマ変換を適用するか。

        bool operator==(const ViewSettings&) const = default;
    };

    struct Request {
        std::string passName = "Composite"; ///< 直後の画像をキャプチャする描画パスの名前。
        size_t occurrence = 0; ///< 同名のパスのうちキャプチャする回を識別する番号。
        std::string outputName; ///< キャプチャする論理出力の名前。
        /// @note outputName が「希望」か「指定」か。false (既定) は指定で、名前が無ければ «選べない» として
        /// @note 状態文字列を出す (綴り違いを黙って別の画像で埋めないため)。true は希望で、無ければそのパス
        /// @note の主出力へ落とす。
        /// @note 分ける理由: Viewer の «出力を固定してパスを送る» は名前を持たないパスを必ず通る
        /// @note (深度固定のまま Composite へ進む等)。毎回エラーだと送りが止まるが、手で選んだ名前が
        /// @note 消えたことは知りたい。
        bool outputIsPreference = false;
        ViewSettings view; ///< このキャプチャに適用する露出と表示方式。
    };

    Request request; ///< 次にプレビューするパス・出力とその表示設定。

    /// @note リソース一覧タブ用のサムネイル 1 枚。
    struct GalleryTile {
        /// @note 宣言 (RenderGraph) と実体 (RenderResourceRegistry) の食い違い。
        /// @note 種類を分ける理由: この 2 つは別々に手で維持されていて、片方だけ抜けても Plan は
        /// @note 通ってしまう。«依存は張られたのに束縛が無い» と «束縛はあるが誰も申告していない» は
        /// @note 原因も直し方も違うため区別して出す。
        enum class Issue {
            None,
            NotBound,     ///< 生きているパスが申告しているのに実体が差さっていない
            NotDeclared,  ///< 実体は差さっているが、このフレームの誰も申告していない
            NoImage       ///< 差さっているが 2D 画像として取り出せない (配列・バッファ等)
        };

        std::string resource;   ///< 論理リソース名
        std::string label;      ///< 表示名 (MRT スライスや深度を含む)
        Issue       issue = Issue::None; ///< パスの申告と実リソースの不一致、または画像を取得できない理由。
        uint32_t    sourceWidth = 0; ///< サムネイル化する前の画像の幅（画素）。
        uint32_t    sourceHeight = 0; ///< サムネイル化する前の画像の高さ（画素）。
        bool        depth = false; ///< 元のリソースが深度画像か。
        bool        hasImage = false; ///< このタイルに表示可能な画像を生成できたか。
        renderer::SizedRenderTarget image; ///< ギャラリーに表示するサムネイルの描画先。
        renderer::ResourceHandle<renderer::ConstantBufferTag> constants; ///< このタイルを表示用に変換するときの定数バッファ。
    };

    /// @note 一覧を焼くかどうか。タブを開いている間だけ true にする。
    bool galleryEnabled = false;
    /// @note サムネイル 1 枚の横幅 [px]。高さは元の縦横比から決める。
    uint32_t galleryTileWidth = 192;
    ViewSettings galleryView; ///< リソース一覧の各サムネイルに適用する共通の表示設定。

    /// @note 描画を飛ばすフレームでも呼び、古い画像を有効なキャプチャとして扱わない。
    FBZZ_GRAPHICS_API void Invalidate();
    FBZZ_GRAPHICS_API void Begin(const std::vector<renderer::RenderGraph::RenderPass>& passes,
               const renderer::RenderGraph::ExecutionReport& report);
    FBZZ_GRAPHICS_API void Capture(size_t graphIndex, RenderPassContext& ctx);
    /// @note 非同期 GPU 値を別フレームの画像へ結合しない。未完了・欠損・出自不一致は gpuMs=-1。
    FBZZ_GRAPHICS_API void Finish(const renderer::RenderGraph::ExecutionReport& report,
                const GpuProfilerSnapshot& gpuSnapshot,
                const GpuProfilerViewMetadata& capturedView);
    /// @note ResourceManager が生存している間に所有者が呼ぶ。
    FBZZ_GRAPHICS_API void Release(renderer::ResourceManager& resources);

    const std::vector<PassInfo>& Passes() const { return m_passes; }
    const std::vector<OutputInfo>& Outputs() const { return m_outputs; }
    const std::vector<GalleryTile>& Gallery() const { return m_gallery; }
    const std::string& Status() const { return m_status; }
    const std::string& GalleryStatus() const { return m_galleryStatus; }
    bool WantsPass(size_t graphIndex) const { return graphIndex == m_selectedIndex; }
    bool HasPreview() const { return m_hasPreview; }
    renderer::ResourceHandle<renderer::RenderTargetTag> Preview() const { return m_preview.Handle(); }
    const Request& CapturedRequest() const { return m_capturedRequest; }

    /// @note 実際に映している出力の名前。outputIsPreference で主出力へ落ちた場合、
    /// @note request.outputName (希望) と食い違う。Viewer はこちらを表示すること。
    const std::string& ResolvedOutputName() const { return m_resolvedOutputName; }
    const ViewSettings& CapturedGalleryView() const { return m_capturedGalleryView; }
    uint32_t Width() const { return m_preview.Width(); }
    uint32_t Height() const { return m_preview.Height(); }

private:
    /// @note 論理リソース名 1 つを «表示できる 2D 画像» へ展開する。
    /// @note RenderTarget なら MRT スライスと (深度を持つなら) 深度、Texture ならそれ 1 枚。
    FBZZ_GRAPHICS_API void CollectImages(RenderPassContext& ctx, const std::string& name,
                       std::vector<OutputInfo>& out) const;

    /// @note 診断シェーダーと PSO を用意する。ResourceManager のリセットを跨いでも作り直す。
    FBZZ_GRAPHICS_API bool EnsurePreviewPipeline(RenderPassContext& ctx);

    FBZZ_GRAPHICS_API void CapturePreview(size_t graphIndex, RenderPassContext& ctx);

    /// @note 画像 1 枚を dst へ焼く。AUTO の解決もここで行う。
    FBZZ_GRAPHICS_API bool BlitPreview(RenderPassContext& ctx, const OutputInfo& output, const ViewSettings& view,
                     renderer::ResourceHandle<renderer::ConstantBufferTag>& constants,
                     renderer::SizedRenderTarget& dst);

    FBZZ_GRAPHICS_API void CaptureGallery(RenderPassContext& ctx);
    FBZZ_GRAPHICS_API void ReleaseGallery(renderer::ResourceManager& resources);

    std::vector<PassInfo> m_passes; ///< 現在のビューで選択できるパスと実行情報。
    std::vector<OutputInfo> m_outputs; ///< 選択したパスから参照できる出力の一覧。
    std::vector<std::string> m_depthTargets; ///< 深度画像として公開されている論理リソース名。
    /// @note このフレームに «生きているパスが触れた» リソース名。宣言側の正本。
    std::vector<std::string> m_declared;
    std::string m_status = "Waiting for the selected view."; ///< 選択パスのプレビュー状態を表示するメッセージ。
    std::string m_galleryStatus; ///< リソース一覧のキャプチャ状態を表示するメッセージ。
    Request m_capturedRequest; ///< 現在のプレビュー画像を生成したときの要求。
    std::string m_resolvedOutputName; ///< 実際にプレビューに使った出力名。
    ViewSettings m_capturedGalleryView; ///< 現在のサムネイル群の生成時に使用した表示設定。
    bool m_hasPreview = false; ///< 選択パスのプレビュー画像を表示できるか。
    size_t m_selectedIndex = static_cast<size_t>(-1); ///< キャプチャ対象のパスを指すグラフ内の添字。
    /// @note 実行順の最後のパス。ここでフレーム末尾の状態として一覧を焼く。
    size_t m_lastExecutedIndex = static_cast<size_t>(-1);
    renderer::SizedRenderTarget m_preview; ///< 選択したパスの出力を表示用に変換した描画ターゲット。
    renderer::ResourceHandle<renderer::ShaderTag> m_shader; ///< リソースを診断表示用の色に変換するシェーダー。
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipelineState; ///< 診断表示の描画に使うパイプライン状態。
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_constants; ///< プレビューの露出と表示範囲を渡す定数バッファ。
    std::vector<GalleryTile> m_gallery; ///< フレーム末尾に取得したリソース別の診断情報とサムネイル。
    uint64_t m_resetVersion = 0; ///< キャプチャ用 GPU 資源を作成した時点の管理器の世代。
};

} // namespace fbzz::renderer
