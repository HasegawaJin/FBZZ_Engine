/// @file    RenderPassCapture.hpp
/// @brief   選択パス直後の画像と実行情報、およびリソース一覧をビュー単位で保持する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// 2 つの見方を 1 つのキャプチャが受け持つ。
///   Preview … 選んだ 1 パスの直後。パスの «途中経過» を見る
///   Gallery … フレーム末尾の全リソース。いま何が差さっているかを «一望» する
/// どちらも同じ診断シェーダーで焼くので、表示モードの意味も同じになる。
#pragma once

#include <Engine/Renderer/RenderGraph.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <string>
#include <vector>

namespace fbzz::renderer { struct GpuPassProfile; }

namespace fbzz::scene {

struct RenderPassContext;

class RenderPassCapture {
public:
    RenderPassCapture() = default;
    RenderPassCapture(const RenderPassCapture&) = delete;
    RenderPassCapture& operator=(const RenderPassCapture&) = delete;

    struct PassInfo {
        std::string name;
        size_t occurrence = 0;
        size_t graphIndex = 0;
        bool culled = false;
        double cpuMs = -1.0;
        double gpuMs = -1.0;
        std::vector<renderer::RenderGraph::ResourceAccess> accesses;
    };

    struct OutputInfo {
        std::string name;
        renderer::ResourceHandle<renderer::TextureTag> texture;
        bool depth = false;
        bool cameraDepth = false;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    enum class DisplayMode { AUTO, RGB, RED, GREEN, BLUE, ALPHA, SIGNED_VECTOR, LINEAR_DEPTH };

    /// 画像 1 枚の «見せ方»。Preview と Gallery が同じものを使う。
    struct ViewSettings {
        DisplayMode mode = DisplayMode::AUTO;
        float exposure = 0.0f;
        float rangeMin = 0.0f;
        float rangeMax = 1.0f;
        bool  gamma = false;

        bool operator==(const ViewSettings&) const = default;
    };

    struct Request {
        std::string passName = "Composite";
        size_t occurrence = 0;
        std::string outputName;
        /// outputName が「希望」か「指定」か。
        ///
        /// false (既定) … 指定。その名前が無ければ «選べない» として状態文字列を出す。
        ///                 綴り違いを黙って別の画像で埋めないための既定。
        /// true          … 希望。無ければそのパスの主出力へ落とす。
        ///
        /// WHY 分けるか: Viewer の «出力を固定してパスを送る» は、名前を持たないパスを
        ///      必ず通る (深度を固定したまま Composite へ進む等)。そこで毎回エラーを
        ///      出すと送り自体が止まる。一方で手で選んだ名前が消えたことは知りたい。
        bool outputIsPreference = false;
        ViewSettings view;
    };

    Request request;

    /// リソース一覧タブ用のサムネイル 1 枚。
    struct GalleryTile {
        /// 宣言 (RenderGraph) と実体 (RenderResourceRegistry) の食い違い。
        ///
        /// WHY 種類を分けるか: この 2 つは別々に手で維持されていて、片方だけ抜けても
        ///     Plan は通ってしまう。«依存は張られたのに束縛が無い» と «束縛はあるが
        ///     誰も申告していない» は原因も直し方も違うので、区別して出す。
        enum class Issue {
            None,
            NotBound,     ///< 生きているパスが申告しているのに実体が差さっていない
            NotDeclared,  ///< 実体は差さっているが、このフレームの誰も申告していない
            NoImage       ///< 差さっているが 2D 画像として取り出せない (配列・バッファ等)
        };

        std::string resource;   ///< 論理リソース名
        std::string label;      ///< 表示名 (MRT スライスや深度を含む)
        Issue       issue = Issue::None;
        uint32_t    sourceWidth = 0;
        uint32_t    sourceHeight = 0;
        bool        depth = false;
        bool        hasImage = false;
        renderer::SizedRenderTarget image;
        renderer::ResourceHandle<renderer::ConstantBufferTag> constants;
    };

    /// 一覧を焼くかどうか。タブを開いている間だけ true にする。
    bool galleryEnabled = false;
    /// サムネイル 1 枚の横幅 [px]。高さは元の縦横比から決める。
    uint32_t galleryTileWidth = 192;
    ViewSettings galleryView;

    /// 描画を飛ばすフレームでも呼び、古い画像を有効なキャプチャとして扱わない。
    void Invalidate();
    void Begin(const std::vector<renderer::RenderGraph::RenderPass>& passes,
               const renderer::RenderGraph::ExecutionReport& report);
    void Capture(size_t graphIndex, RenderPassContext& ctx);
    void Finish(const renderer::RenderGraph::ExecutionReport& report,
                const std::vector<renderer::GpuPassProfile>& gpuTimings);
    /// ResourceManager が生存している間に所有者が呼ぶ。
    void Release(renderer::ResourceManager& resources);

    const std::vector<PassInfo>& Passes() const { return m_passes; }
    const std::vector<OutputInfo>& Outputs() const { return m_outputs; }
    const std::vector<GalleryTile>& Gallery() const { return m_gallery; }
    const std::string& Status() const { return m_status; }
    const std::string& GalleryStatus() const { return m_galleryStatus; }
    bool WantsPass(size_t graphIndex) const { return graphIndex == m_selectedIndex; }
    bool HasPreview() const { return m_hasPreview; }
    renderer::ResourceHandle<renderer::RenderTargetTag> Preview() const { return m_preview.Handle(); }
    const Request& CapturedRequest() const { return m_capturedRequest; }

    /// 実際に映している出力の名前。outputIsPreference で主出力へ落ちた場合、
    /// request.outputName (希望) と食い違う。Viewer はこちらを表示すること。
    const std::string& ResolvedOutputName() const { return m_resolvedOutputName; }
    const ViewSettings& CapturedGalleryView() const { return m_capturedGalleryView; }
    uint32_t Width() const { return m_preview.Width(); }
    uint32_t Height() const { return m_preview.Height(); }

private:
    /// 論理リソース名 1 つを «表示できる 2D 画像» へ展開する。
    /// RenderTarget なら MRT スライスと (深度を持つなら) 深度、Texture ならそれ 1 枚。
    void CollectImages(RenderPassContext& ctx, const std::string& name,
                       std::vector<OutputInfo>& out) const;

    /// 診断シェーダーと PSO を用意する。ResourceManager のリセットを跨いでも作り直す。
    bool EnsurePreviewPipeline(RenderPassContext& ctx);

    void CapturePreview(size_t graphIndex, RenderPassContext& ctx);

    /// 画像 1 枚を dst へ焼く。AUTO の解決もここで行う。
    bool BlitPreview(RenderPassContext& ctx, const OutputInfo& output, const ViewSettings& view,
                     renderer::ResourceHandle<renderer::ConstantBufferTag>& constants,
                     renderer::SizedRenderTarget& dst);

    void CaptureGallery(RenderPassContext& ctx);
    void ReleaseGallery(renderer::ResourceManager& resources);

    std::vector<PassInfo> m_passes;
    std::vector<OutputInfo> m_outputs;
    std::vector<std::string> m_depthTargets;
    /// このフレームに «生きているパスが触れた» リソース名。宣言側の正本。
    std::vector<std::string> m_declared;
    std::string m_status = "Waiting for the selected view.";
    std::string m_galleryStatus;
    Request m_capturedRequest;
    std::string m_resolvedOutputName;
    ViewSettings m_capturedGalleryView;
    bool m_hasPreview = false;
    size_t m_selectedIndex = static_cast<size_t>(-1);
    /// 実行順の最後のパス。ここでフレーム末尾の状態として一覧を焼く。
    size_t m_lastExecutedIndex = static_cast<size_t>(-1);
    renderer::SizedRenderTarget m_preview;
    renderer::ResourceHandle<renderer::ShaderTag> m_shader;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipelineState;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_constants;
    std::vector<GalleryTile> m_gallery;
    uint64_t m_resetVersion = 0;
};

} // namespace fbzz::scene
