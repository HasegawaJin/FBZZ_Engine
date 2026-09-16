/// @file    ResourceManager.hpp
/// @brief   Renderer リソースの所有とハンドル解決。
/// @author  Hasegawa Jin
/// @date    2026-05-22
///
/// IRenderer の非公開生成 API を呼べる唯一の窓口。
/// 実体は ResourcePool が unique_ptr で単独所有し、上位システムは ResourceHandle だけを持つ。
#pragma once
#include <Engine/Core/Memory/AllocationInfo.hpp>
#include <Engine/Renderer/Format.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ResourcePool.hpp>
#include <Engine/Renderer/RenderState.hpp>
// WHY: DynamicTextureFormat を CreateDynamicTexture の引数に取るため、
//      ITexture の前方宣言だけでは足りず実体が要る (ヘッダ自体は cstdint のみに依存する軽量なもの)。
#include <Engine/Renderer/ITexture.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

class IBuffer;
class IConstantBuffer;
class IPipelineState;
class IRenderTarget;
class IRenderer;
class IShader;
class IStructuredBuffer;
class ITexture;
struct Mesh;

class ResourceManager {
public:
    explicit ResourceManager(IRenderer& renderer);
    ~ResourceManager();
    // Application が初期化時に登録する唯一のインスタンス。
    // System から ResourceManager& を受け取れない場面で使うフォールバック。
    static ResourceManager* Active();

    ResourceHandle<ShaderTag> LoadShader(std::string_view path);

    /// レンダースレッドのフレーム外で呼ぶ。失敗時は旧実体とハンドルを保持する。
    ResourceHandle<ShaderTag> ReloadShader(std::string_view path);

    /// 全候補を生成してから一括で差し替える。失敗時は旧状態を保持して false。
    /// レンダースレッドのフレーム外で呼ぶ。
    bool ReloadAllShaders();
    [[nodiscard]] uint64_t GetShaderVersion() const { return m_shaderVersion; }
    // デバイスロスト復帰用に全 GPU リソースを破棄し、次フレームの遅延再生成を促す。
    // WHY: 旧 D3D デバイスに紐づく COM リソースは新デバイスで再利用できないため。
    void Reset();
    [[nodiscard]] uint64_t GetResetVersion() const { return m_resetVersion; }
    ResourceHandle<TextureTag> LoadTexture(std::string_view path);
    // 同名ファイルを上書きした際、既存ハンドルを維持したまま GPU テクスチャを差し替える。
    // WHY: IBL ベイク後もパスキャッシュが古い DDS を返し続けるため、明示的な再ロードが必要。
    ResourceHandle<TextureTag> ReloadTexture(std::string_view path);

    // path (またはそのフォルダ配下) のテクスチャをキャッシュから外し、GPU 実体を解放する。
    //
    // WHY 必要か: LoadTexture はパスキャッシュに当たった時点で即返すため、ファイルを
    //     消してもエディタを再起動するまで古い絵が出続ける。「消したのに映っている」は
    //     参照切れより質の悪い症状で、消したつもりのアセットを配布物へ持ち込む。
    // NOTE: 既に配られたハンドルは無効になる。参照側は次のフレームで解決し直す前提
    //       (UIImage / MaterialComponent は loadedTexturePath を見て再解決する)。
    // @param path Assets/ 起点の相対パス。フォルダを渡すと配下をまとめて外す。
    // @ret 外したエントリ数。
    std::size_t EvictTexture(std::string_view path);
    ResourceHandle<TextureTag> CreateTexture(const uint8_t* rgba, uint32_t width, uint32_t height,
                                             Where where = Where::current());
    /// テクスチャ未設定のフォールバックが共有する 1x1 白テクスチャ。
    /// WHY: 呼ぶたびに CreateTexture すると «誰も返さない 1 枚» が実体ごとに増える。
    ///      パーティクル / トレイルは寿命が短く数も多いので、そのぶんだけ漏れ続ける。
    [[nodiscard]] ResourceHandle<TextureTag> GetWhiteTexture();
    // CPU生成したRGBA8ボリュームから3D Textureを作る。Color LUTなどに使用する。
    ResourceHandle<TextureTag> CreateTexture3D(
        const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth,
        Where where = Where::current());

    ResourceHandle<BufferTag> CreateVertexBuffer(const void* data, size_t bytes, uint32_t stride,
                                                 Where where = Where::current());
    // CS が書き込み、IA が頂点として読むバッファ (コンピュートスキニングの出力先)。
    // 未対応バックエンドでは無効ハンドルが返る。呼び出し側は VS スキニングへフォールバックすること。
    ResourceHandle<BufferTag> CreateGpuWritableVertexBuffer(size_t bytes, uint32_t stride,
                                                            Where where = Where::current());
    ResourceHandle<BufferTag> CreateIndexBuffer(const void* data, uint32_t count,
                                                Where where = Where::current());
    ResourceHandle<ConstantBufferTag> CreateConstantBuffer(size_t sizeBytes,
                                                           Where where = Where::current());
    ResourceHandle<PipelineStateTag> CreatePipelineState(const PipelineStateDesc& desc,
                                                          Where where = Where::current());
    /// 形式と深度の有無を明示して生成する。
    /// @note colorCount = 0 は深度専用。withDepth = false かつ colorCount = 0 は作れない。
    ResourceHandle<RenderTargetTag> CreateRenderTarget(uint32_t width, uint32_t height,
                                                       const RenderTargetDesc& desc,
                                                       Where where = Where::current());
    /// 従来どおり RGBA16F + 深度付きで生成する短縮形。
    ResourceHandle<RenderTargetTag> CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount = 1,
                                                       Where where = Where::current());
    // 6 面キューブマップ描画先を生成する (空連動 IBL の SkyCapture 用)。
    // 面の描画は IRenderer::SetRenderTargetFace、サンプリングは GetCubemapTexture() で取得した
    // TextureTag を DrawCall.textures[] へ束縛して行う。未対応バックエンドでは Null を返す。
    ResourceHandle<RenderTargetTag> CreateCubemapRenderTarget(uint32_t size, uint32_t mipCount = 1,
                                                              Where where = Where::current());
    ResourceHandle<TextureTag> CreateComputeTexture(uint32_t width, uint32_t height,
                                                    Where where = Where::current());
    // CS が RWTexture3D として書き、後段が Texture3D として読むボリューム (フロクセル霧)。
    // 未対応バックエンドでは Null ハンドルが返る。呼び出し側は機能ごと落とすこと。
    ResourceHandle<TextureTag> CreateComputeTexture3D(
        uint32_t width, uint32_t height, uint32_t depth,
        Where where = Where::current());
    // CPU から矩形単位で書き換えられるテクスチャ (ゼロクリア済み) を作る。
    // 更新は Get(handle)->UpdateRegion(...) で行う。フォントの動的アトラスが使う。
    // 未対応バックエンドでは Null ハンドルが返る。
    ResourceHandle<TextureTag> CreateDynamicTexture(
        uint32_t width, uint32_t height, DynamicTextureFormat format,
        Where where = Where::current());
    // 外部 (IRenderer) が生成済みの ITexture を ResourcePool に引き取り TextureTag を返す。
    // WHY: 空連動 IBL の BakeSkyLight が畳み込んだキューブ SRV ラッパーを Lit パスへ束縛可能にする。
    ResourceHandle<TextureTag> RegisterTexture(std::unique_ptr<ITexture> texture,
                                               Where where = Where::current());
    // GPU Instancing 用 StructuredBuffer。elementCount 個・stride バイトの DYNAMIC バッファを作成する。
    ResourceHandle<StructuredBufferTag> CreateStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride,
                                                               Where where = Where::current());
    // 初期データを一度だけ転送し、GPU ローカルな読み取り専用 SRV として保持する。
    // WHY: 毎フレーム不変なスキニング入力を UPLOAD Heap から読むと PCIe/UMA 経路が律速になるため。
    ResourceHandle<StructuredBufferTag> CreateGpuLocalStructuredBuffer(
        const void* data, uint32_t elementCount, uint32_t stride,
        Where where = Where::current());
    // CS が RWStructuredBuffer として書き込む DEFAULT バッファ (SRV + UAV)。GPU パーティクルプール等に使う。
    ResourceHandle<StructuredBufferTag> CreateRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride,
                                                                 Where where = Where::current());

    IShader* Get(ResourceHandle<ShaderTag> h);
    ITexture* Get(ResourceHandle<TextureTag> h);
    IBuffer* Get(ResourceHandle<BufferTag> h);
    IConstantBuffer* Get(ResourceHandle<ConstantBufferTag> h);
    IPipelineState* Get(ResourceHandle<PipelineStateTag> h);
    IRenderTarget* Get(ResourceHandle<RenderTargetTag> h);
    IStructuredBuffer* Get(ResourceHandle<StructuredBufferTag> h);

    ResourceHandle<TextureTag> GetColorTexture(ResourceHandle<RenderTargetTag> rt, uint32_t index = 0);
    ResourceHandle<TextureTag> GetDepthTexture(ResourceHandle<RenderTargetTag> rt);
    // キューブマップ RT の TextureCube SRV ハンドル (CreateCubemapRenderTarget で作った RT 用)。
    ResourceHandle<TextureTag> GetCubemapTexture(ResourceHandle<RenderTargetTag> rt);

    void Update(ResourceHandle<BufferTag> h, const void* data, size_t sizeBytes);
    void Update(ResourceHandle<ConstantBufferTag> h, const void* data, size_t sizeBytes);
    void Update(ResourceHandle<StructuredBufferTag> h, const void* data, size_t sizeBytes);

    void Release(ResourceHandle<ShaderTag> h);
    void Release(ResourceHandle<TextureTag> h);
    void Release(ResourceHandle<BufferTag> h);
    void Release(ResourceHandle<ConstantBufferTag> h);
    void Release(ResourceHandle<PipelineStateTag> h);
    void Release(ResourceHandle<RenderTargetTag> h);
    void Release(ResourceHandle<StructuredBufferTag> h);

    [[nodiscard]] std::size_t GetLiveDebugResourceCount() const;
    /// 生存中のリソース追跡情報を全プールぶん out へ追記する。
    ///
    /// WHY 索引で 1 件ずつ取る API を置かないか: 台帳は疎な固定長配列なので、索引指定は
    ///     1 件ごとに先頭から数え直すことになる。全件を舐める用途 (一覧・差分・終了時の
    ///     集計) しか無いのに、それを許すと必ず本数の 2 乗のループが書かれる。
    void CollectLiveDebugResources(std::vector<core::AllocationInfo>& out) const;
    /// 生存中リソースの申告バイト合計 (O(1))。
    [[nodiscard]] std::size_t GetLiveDebugResourceBytes() const;

    /// フレームごとの増加を見張り、増え続けたら発生位置つきで警告する。フレームに 1 回呼ぶ。
    ///
    /// WHY 自動で見張るか: 「毎フレーム少しずつ増える」類は、パネルを開いて見比べない限り
    ///     気付けない。1 分遊べば数百 MB になるものを «気付いた人だけが直す» 形にしない。
    /// NOTE: 検証構成 (FBZZ_GPU_VALIDATION) でだけ動く。Release では何もしない。
    void TickLeakWatchdog();

    /// Mesh の頂点 / インデックスバッファを返し、ハンドルを空にする。@ret 返した本数。
    ///
    /// WHY Mesh 自身に返させないか: Mesh は «形» を持つだけの構造体で ResourceManager を
    ///     知らない。持たせると、マネージャーより長生きする静的キャッシュ上の Mesh が
    ///     終了後に返しにいく。所有者 (Model / ProceduralMesh などの Component) が
    ///     畳まれる場所で明示的に呼ぶ。
    std::size_t ReleaseMeshBuffers(Mesh& mesh);

private:
    static uint64_t Key(ResourceHandle<RenderTargetTag> h);
    void ReleaseOwnedResourcesForShutdown();
    void LogLiveDebugResources() const;

    IRenderer& m_renderer;

    ResourcePool<IShader, ShaderTag> m_shaders;
    ResourcePool<ITexture, TextureTag> m_textures;
    ResourcePool<IBuffer, BufferTag> m_buffers;
    ResourcePool<IConstantBuffer, ConstantBufferTag> m_constantBuffers;
    ResourcePool<IPipelineState, PipelineStateTag> m_pipelineStates;
    ResourcePool<IRenderTarget, RenderTargetTag> m_renderTargets;
    ResourcePool<IStructuredBuffer, StructuredBufferTag> m_structuredBuffers;

    std::unordered_map<std::string, ResourceHandle<ShaderTag>> m_shaderCache;
    std::unordered_map<std::string, ResourceHandle<TextureTag>> m_textureCache;
    std::unordered_map<uint64_t, std::vector<ResourceHandle<TextureTag>>> m_renderTargetColors;
    std::unordered_map<uint64_t, ResourceHandle<TextureTag>> m_renderTargetDepths;
    ResourceHandle<TextureTag> m_whiteTexture;
    uint64_t m_resetVersion = 1;
    uint64_t m_shaderVersion = 1;

    // リーク見張りの状態。前フレームの合計と «増え続けた連続フレーム数»。
    std::size_t m_watchdogLastBytes = 0;
    uint64_t    m_watchdogFrame = UINT64_MAX;
    uint32_t    m_watchdogGrowthFrames = 0;
    uint32_t    m_watchdogReportCount = 0;
};

/// 寸法に合わせて作り直すレンダーターゲット。
///
/// WHY これを挟むか:
///   「今の寸法を覚えておく」「変わったら *前のを返してから* 作り直す」「デバイスリセットの
///   ときだけは返さずに作り直す」の 3 つは、オフスクリーン RT を持つパスすべてで同じ形に
///   なる。各所で静的変数 (ハンドル + 幅 + 高さ + リセット版数) を並べて書いていたが、
///   1 か所でも «返す» を書き落とすと、寸法が動くたびに RT が丸ごと GPU に残る。
///   実際 CSM のアトラスがそれで漏れ、«ShadowPass だけ突然重い / エディタ再起動で直る»
///   として出た (2026-09-01)。書き落としようのない形へ寄せる。
///
/// 使い方は毎フレーム `Ensure(...)` を呼ぶだけ。寸法が同じなら何もしない。
/// ハンドルへ暗黙変換できるので、描画側 (SetRenderTarget / GetDepthTexture) はそのまま渡せる。
class SizedRenderTarget {
public:
    /// @param colorCount 0 = 深度のみ。
    /// @ret 今回作り直したら true (中身は未定義になるので、キャッシュを持つ側は捨てること)。
    /// @note where は既定のまま渡すこと。RT の発生位置が «この Ensure» ではなく
    ///       «どのパスが持っている RT か» として記録される (Where の WHY を参照)。
    bool Ensure(ResourceManager& resources, uint32_t width, uint32_t height, uint32_t colorCount = 1,
                Where where = Where::current());
    /// 明示的に返す。以後の Ensure は作り直しから始まる。
    void Release(ResourceManager& resources);

    [[nodiscard]] ResourceHandle<RenderTargetTag> Handle() const { return m_handle; }
    operator ResourceHandle<RenderTargetTag>() const { return m_handle; }
    [[nodiscard]] bool     IsValid() const { return m_handle.IsValid(); }
    [[nodiscard]] uint32_t Width()   const { return m_width; }
    [[nodiscard]] uint32_t Height()  const { return m_height; }

private:
    ResourceHandle<RenderTargetTag> m_handle;
    uint32_t m_width      = 0;
    uint32_t m_height     = 0;
    uint32_t m_colorCount = 0;
    // ResourceManager の初期値は 1。0 から始めることで初回は必ず «リセット後» として通る。
    uint64_t m_resetVersion = 0;
};

} // namespace fbzz::renderer
