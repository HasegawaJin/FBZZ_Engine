/// @file    ResourceManager.hpp
/// @brief   Renderer リソースの所有とハンドル解決。
/// @author  Hasegawa Jin
/// @date    2026-05-22
/// @note IRenderer の非公開生成 API を呼べる唯一の窓口。
/// @note 実体は ResourcePool が unique_ptr で単独所有し、上位システムは ResourceHandle だけを持つ。
#pragma once
#include <Core/Memory/AllocationInfo.hpp>
#include <Graphics/Renderer/Format.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Graphics/Renderer/ResourcePool.hpp>
#include <Graphics/Renderer/RenderState.hpp>
/// @note DynamicTextureFormat を CreateDynamicTexture の引数に取るため、ITexture の前方宣言
/// @note       だけでは足りず実体が要る (ヘッダ自体は cstdint のみに依存する軽量なもの)。
#include <Graphics/Renderer/ITexture.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

class RenderResources;

class ResourceManager {
public:
    explicit ResourceManager(IRenderer& renderer);
    ~ResourceManager();
    /// @note 描画状態は Manager ごとに独立し、Reset で GPU 実体とともに破棄する。
    RenderResources& Rendering();
    void ReleaseRenderView(uint32_t key);
    /// @note ホストの描画フレーム開始時に一度呼ぶ。Play のリセットでは巻き戻さない。
    void AdvanceFrame() { ++m_frameStamp; }
    [[nodiscard]] uint64_t FrameStamp() const { return m_frameStamp; }
    /// @note Application が初期化時に登録する唯一のインスタンス。
    /// @note System から ResourceManager& を受け取れない場面で使うフォールバック。
    static ResourceManager* Active();

    ResourceHandle<ShaderTag> LoadShader(std::string_view path);

    /// @note レンダースレッドのフレーム外で呼ぶ。失敗時は旧実体とハンドルを保持する。
    ResourceHandle<ShaderTag> ReloadShader(std::string_view path);

    /// @note 全候補を生成してから一括で差し替える。失敗時は旧状態を保持して false。
    /// @note レンダースレッドのフレーム外で呼ぶ。
    bool ReloadAllShaders();
    [[nodiscard]] uint64_t GetShaderVersion() const { return m_shaderVersion; }
    /// @note デバイスロスト復帰用に全 GPU リソースを破棄し、次フレームの遅延再生成を促す。
    /// @note 旧 D3D デバイスに紐づく COM リソースは新デバイスで再利用できないため。
    void Reset();
    [[nodiscard]] uint64_t GetResetVersion() const { return m_resetVersion; }
    ResourceHandle<TextureTag> LoadTexture(std::string_view path);
    /// @note 同名ファイルを上書きした際、既存ハンドルを維持したまま GPU テクスチャを差し替える。
    /// @note IBL ベイク後もパスキャッシュが古い DDS を返し続けるため、明示的な再ロードが必要。
    ResourceHandle<TextureTag> ReloadTexture(std::string_view path);

    /// @note path (またはそのフォルダ配下) のテクスチャをキャッシュから外し、GPU 実体を解放する。
    /// @note LoadTexture はパスキャッシュに当たった時点で即返すため、ファイルを消しても
    /// @note       エディタを再起動するまで古い絵が出続ける。消したつもりのアセットが配布物へ
    /// @note       持ち込まれるのを防ぐために必要。
    /// @note 既に配られたハンドルは無効になる。参照側は次のフレームで解決し直す前提
    /// @note       (UIImage / MaterialComponent は loadedTexturePath を見て再解決する)。
    /// @param path Assets/ 起点の相対パス。フォルダを渡すと配下をまとめて外す。
    /// @return 外したエントリ数。
    std::size_t EvictTexture(std::string_view path);
    ResourceHandle<TextureTag> CreateTexture(const uint8_t* rgba, uint32_t width, uint32_t height,
                                             Where where = Where::current());
    /// @note CPU で焼いたミップ連鎖 (RGBA8) からテクスチャを作る。
    /// @param mips 0 段目から順。空、または未対応バックエンドでは無効ハンドルを返す。
    /// @note 縮小を CPU 側で決めたいテクスチャ (水面のさざ波勾配タイルなど) 用。
    ResourceHandle<TextureTag> CreateTextureWithMips(const TextureMipData* mips, uint32_t mipCount,
                                                     Where where = Where::current());

    /// @name 非同期テクスチャ転送
    /// @see Docs/design/asset-streaming.md «GPU 転送と公開»
    /// @{
    /// @brief CreateTextureWithMips の待たない版。返したハンドルはパスキャッシュに載らない。
    /// @param outUploadToken IsUploadComplete に渡す値。0 は完了済み。
    /// @note 完了前のハンドルを描画へ渡さない。公開は完了確認後に PublishTexture で行う。
    /// @note 完了前に Release してよい。GPU 実体の返却はバックエンドのフェンス管理が遅らせる。
    ResourceHandle<TextureTag> BeginTextureUpload(const TextureMipData* mips, uint32_t mipCount,
                                                  uint64_t& outUploadToken,
                                                  Where where = Where::current());
    /// @brief BeginTextureUpload の転送が GPU 上で終わったか。CPU は待たない。
    [[nodiscard]] bool IsUploadComplete(uint64_t uploadToken) const;
    /// @brief パスキャッシュに載っているテクスチャを引く。読み込みはしない。
    [[nodiscard]] ResourceHandle<TextureTag> FindCachedTexture(std::string_view path) const;
    /// @brief 転送を終えたテクスチャを path のキャッシュへ載せ、以後の LoadTexture が同じ実体を返すようにする。
    /// @return キャッシュに載ったハンドル。既に同じ path が載っていれば既存を返し、uploaded は返却する。
    ResourceHandle<TextureTag> PublishTexture(std::string_view path, ResourceHandle<TextureTag> uploaded);
    /// @brief uploaded の実体を target のスロットへ移し、target の旧実体を返す (品質変更の差し替え)。
    /// @note target のハンドルは有効なまま中身だけが変わる。旧実体の GPU 返却と bindless 枠の回収は
    /// @note       バックエンドのフェンス管理が遅らせるので、記録済みの描画が読み終えるまで生きる。
    /// @return 移せなければ false (uploaded はそのまま)。
    bool ReplaceTextureContents(ResourceHandle<TextureTag> target, ResourceHandle<TextureTag> uploaded);
    /// @brief LoadTexture と同じ読み込みとキャッシュだが、«外部に配った» 印を付けない。
    /// @note 非同期経路 (TextureAsset) だけが使う。印の無い実体は EvictStreamedTexture で外せる。
    ResourceHandle<TextureTag> LoadTextureUnpinned(std::string_view path);
    /// @brief 非同期経路が載せたテクスチャをキャッシュから外して返す。
    /// @return LoadTexture で配られたことがある (誰かがハンドルを持っているかもしれない) なら false で何もしない。
    bool EvictStreamedTexture(std::string_view path);
    /// @brief テクスチャ 1 枚の概算バイト数 (RGBA8 換算)。無効なハンドルは 0。
    [[nodiscard]] std::size_t GetTextureBytes(ResourceHandle<TextureTag> handle) const;
    /// @}
    /// @note テクスチャ未設定のフォールバックが共有する 1x1 白テクスチャ。
    /// @note 呼ぶたびに CreateTexture すると «誰も返さない 1 枚» が実体ごとに増える。
    /// @note       パーティクル / トレイルは寿命が短く数も多いので、そのぶんだけ漏れ続ける。
    [[nodiscard]] ResourceHandle<TextureTag> GetWhiteTexture();
    /// @note CPU生成したRGBA8ボリュームから3D Textureを作る。Color LUTなどに使用する。
    ResourceHandle<TextureTag> CreateTexture3D(
        const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth,
        Where where = Where::current());

    ResourceHandle<BufferTag> CreateVertexBuffer(const void* data, size_t bytes, uint32_t stride,
                                                 Where where = Where::current());
    /// @note CS が書き込み、IA が頂点として読むバッファ (コンピュートスキニングの出力先)。
    /// @note 未対応バックエンドでは無効ハンドルが返る。呼び出し側は VS スキニングへフォールバックすること。
    ResourceHandle<BufferTag> CreateGpuWritableVertexBuffer(size_t bytes, uint32_t stride,
                                                            Where where = Where::current());
    ResourceHandle<BufferTag> CreateIndexBuffer(const void* data, uint32_t count,
                                                Where where = Where::current());
    ResourceHandle<ConstantBufferTag> CreateConstantBuffer(size_t sizeBytes,
                                                           Where where = Where::current());
    ResourceHandle<PipelineStateTag> CreatePipelineState(const PipelineStateDesc& desc,
                                                          Where where = Where::current());
    /// @note 形式と深度の有無を明示して生成する。
    /// @note colorCount = 0 は深度専用。withDepth = false かつ colorCount = 0 は作れない。
    ResourceHandle<RenderTargetTag> CreateRenderTarget(uint32_t width, uint32_t height,
                                                       const RenderTargetDesc& desc,
                                                       Where where = Where::current());
    /// @note 従来どおり RGBA16F + 深度付きで生成する短縮形。
    ResourceHandle<RenderTargetTag> CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount = 1,
                                                       Where where = Where::current());
    /// @note 6 面キューブマップ描画先を生成する (空連動 IBL の SkyCapture 用)。
    /// @note 面の描画は IRenderer::SetRenderTargetFace、サンプリングは GetCubemapTexture() で取得した
    /// @note TextureTag を DrawCall.textures[] へ束縛して行う。未対応バックエンドでは Null を返す。
    ResourceHandle<RenderTargetTag> CreateCubemapRenderTarget(uint32_t size, uint32_t mipCount = 1,
                                                              Where where = Where::current());
    ResourceHandle<TextureTag> CreateComputeTexture(uint32_t width, uint32_t height,
                                                    Where where = Where::current());
    /// @note CS が RWTexture3D として書き、後段が Texture3D として読むボリューム (フロクセル霧)。
    /// @note 未対応バックエンドでは Null ハンドルが返る。呼び出し側は機能ごと落とすこと。
    ResourceHandle<TextureTag> CreateComputeTexture3D(
        uint32_t width, uint32_t height, uint32_t depth,
        Where where = Where::current());
    /// @note CPU から矩形単位で書き換えられるテクスチャ (ゼロクリア済み) を作る。
    /// @note 更新は Get(handle)->UpdateRegion(...) で行う。フォントの動的アトラスが使う。
    /// @note 未対応バックエンドでは Null ハンドルが返る。
    ResourceHandle<TextureTag> CreateDynamicTexture(
        uint32_t width, uint32_t height, DynamicTextureFormat format,
        Where where = Where::current());
    /// @note 外部 (IRenderer) が生成済みの ITexture を ResourcePool に引き取り TextureTag を返す。
    /// @note 空連動 IBL の BakeSkyLight が畳み込んだキューブ SRV ラッパーを Lit パスへ束縛可能にする。
    ResourceHandle<TextureTag> RegisterTexture(std::unique_ptr<ITexture> texture,
                                               Where where = Where::current());
    /// @note GPU Instancing 用 StructuredBuffer。elementCount 個・stride バイトの DYNAMIC バッファを作成する。
    ResourceHandle<StructuredBufferTag> CreateStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride,
                                                               Where where = Where::current());
    /// @note 初期データを一度だけ転送し、GPU ローカルな読み取り専用 SRV として保持する。
    /// @note 毎フレーム不変なスキニング入力を UPLOAD Heap から読むと PCIe/UMA 経路が律速になるため。
    ResourceHandle<StructuredBufferTag> CreateGpuLocalStructuredBuffer(
        const void* data, uint32_t elementCount, uint32_t stride,
        Where where = Where::current());
    /// @note CS が RWStructuredBuffer として書き込む DEFAULT バッファ (SRV + UAV)。GPU パーティクルプール等に使う。
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
    /// @note キューブマップ RT の TextureCube SRV ハンドル (CreateCubemapRenderTarget で作った RT 用)。
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
    /// @note 生存中のリソース追跡情報を全プールぶん out へ追記する。
    /// @note 台帳は疎な固定長配列なので、索引で 1 件ずつ取る API は先頭から数え直すことになる。
    /// @note       全件を舐める用途 (一覧・差分・終了時の集計) しか無いので、それを許すと必ず
    /// @note       本数の 2 乗のループが書かれる。
    void CollectLiveDebugResources(std::vector<core::AllocationInfo>& out) const;
    /// @note 生存中リソースの申告バイト合計 (O(1))。
    [[nodiscard]] std::size_t GetLiveDebugResourceBytes() const;

    /// @note フレームごとの増加を見張り、増え続けたら発生位置つきで警告する。フレームに 1 回呼ぶ。
    /// @note 「毎フレーム少しずつ増える」類はパネルを開いて見比べない限り気付けないため、
    /// @note       自動で見張る (1 分遊べば数百 MB になるものを «気付いた人だけが直す» 形にしない)。
    /// @note 検証構成 (FBZZ_GPU_VALIDATION) でだけ動く。Release では何もしない。
    void TickLeakWatchdog();

    /// @note Mesh の頂点 / インデックスバッファを返し、ハンドルを空にする。@return 返した本数。
    /// @note Mesh は «形» を持つだけの構造体で ResourceManager を知らない。持たせると、
    /// @note       マネージャーより長生きする静的キャッシュ上の Mesh が終了後に返しにいく。
    /// @note       所有者 (Model / ProceduralMesh などの Component) が畳まれる場所で明示的に呼ぶ。
    std::size_t ReleaseMeshBuffers(Mesh& mesh);

private:
    std::unique_ptr<RenderResources> m_renderResources;
    uint64_t m_frameStamp = 0;
    static uint64_t Key(ResourceHandle<RenderTargetTag> h);
    ResourceHandle<TextureTag> LoadTextureImpl(std::string_view path, bool pin);
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
    /// @note LoadTexture で外へ配ったキー。配られた先がハンドルを持ち続けるかもしれないので、非同期側は外さない。
    std::unordered_set<std::string> m_texturesHandedOut;
    std::unordered_map<uint64_t, std::vector<ResourceHandle<TextureTag>>> m_renderTargetColors;
    std::unordered_map<uint64_t, ResourceHandle<TextureTag>> m_renderTargetDepths;
    ResourceHandle<TextureTag> m_whiteTexture;
    uint64_t m_resetVersion = 1;
    uint64_t m_shaderVersion = 1;

    /// @note リーク見張りの状態。前フレームの合計と «増え続けた連続フレーム数»。
    std::size_t m_watchdogLastBytes = 0;
    uint64_t    m_watchdogFrame = UINT64_MAX;
    uint32_t    m_watchdogGrowthFrames = 0;
    uint32_t    m_watchdogReportCount = 0;
};

/// @note 寸法に合わせて作り直すレンダーターゲット。
/// @note 「今の寸法を覚える」「変わったら *前のを返してから* 作り直す」「デバイスリセットの
/// @note       ときだけは返さずに作り直す」の 3 つは、オフスクリーン RT を持つパスすべてで同じ形に
/// @note       なる。各所で静的変数を並べて書くと 1 か所でも «返す» を書き落とし、寸法が動くたびに
/// @note       RT が丸ごと GPU に残る漏れが起こるため、書き落としようのない形へ寄せる。
/// @note 使い方は毎フレーム `Ensure(...)` を呼ぶだけ。寸法が同じなら何もしない。
/// @note ハンドルへ暗黙変換できるので、描画側 (SetRenderTarget / GetDepthTexture) はそのまま渡せる。
class SizedRenderTarget {
public:
    /// @param colorCount 0 = 深度のみ。
    /// @return 今回作り直したら true (中身は未定義になるので、キャッシュを持つ側は捨てること)。
    /// @note where は既定のまま渡すこと。RT の発生位置が «この Ensure» ではなく
    /// @note       «どのパスが持っている RT か» として記録される (理由は Where を参照)。
    bool Ensure(ResourceManager& resources, uint32_t width, uint32_t height, uint32_t colorCount = 1,
                Where where = Where::current());
    /// @brief 形式・深度の有無・深度の向きまで指定する版。desc が変わっても作り直す。
    bool Ensure(ResourceManager& resources, uint32_t width, uint32_t height, const RenderTargetDesc& desc,
                Where where = Where::current());
    /// @note 明示的に返す。以後の Ensure は作り直しから始まる。
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
    RenderTargetDesc m_desc{};
    /// @note ResourceManager の初期値は 1。0 から始めることで初回は必ず «リセット後» として通る。
    uint64_t m_resetVersion = 0;
};

} /// @note namespace fbzz::renderer
