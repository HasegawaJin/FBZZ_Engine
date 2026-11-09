/// @file    ResourceManager.hpp
/// @brief   Renderer リソースの所有とハンドル解決。
/// @author  Hasegawa Jin
/// @date    2026-05-22
/// @note IRenderer の非公開生成 API を呼べる唯一の窓口。
/// @note 実体は ResourcePool が unique_ptr で単独所有し、上位システムは ResourceHandle だけを持つ。
#pragma once
#include <Graphics/Api.hpp>
#include <Core/Memory/AllocationInfo.hpp>
#include <Graphics/Renderer/Format.hpp>
#include <Graphics/Renderer/AccelerationStructure.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Graphics/Renderer/ShaderCapabilities.hpp>
#include <Graphics/Renderer/ResourcePool.hpp>
#include <Graphics/Renderer/RenderState.hpp>
#include <Graphics/Renderer/MeshletGeometry.hpp>
/// @note DynamicTextureFormat を CreateDynamicTexture の引数に取るため、ITexture の前方宣言
/// @note       だけでは足りず実体が要る (ヘッダ自体は cstdint のみに依存する軽量なもの)。
#include <Graphics/Renderer/ITexture.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <source_location>
#include <span>
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
class Material;
namespace material_transfer_detail { class OwnedConstantBufferData; class MaterialTransferPayloadScope; }
namespace buffer_state_detail { class BufferStateReadScope; }
namespace prepared_skinning_detail { struct TicketFactory; }

class RenderResources;

/// @brief Non-blocking metadata snapshot for a buffer handle.
struct BufferResourceMetadata {
    uint32_t generation = 0;
    size_t sizeBytes = 0;
    uint32_t stride = 0;
    /// @note Advances on ResourceManager CPU updates; GPU-writable output buffers are excluded.
    uint64_t contentVersion = 0;
    bool cpuReadable = false;
    bool gpuWritable = false;
    bool indexBuffer = false;
    bool live = false;
};

/// @brief Native buffer values read after queued CPU work has completed.
/// @note Unlike BufferResourceMetadata, contentVersion includes recorded GPU writes; no resource pointer is retained.
struct BufferResourceState {
    size_t sizeBytes = 0;
    uint32_t stride = 0;
    uint64_t contentVersion = 0;
    uint32_t bindlessUavIndex = UINT32_MAX;
    bool live = false;
};

/// @brief ResourceManager を通る同期テクスチャ読み込みの累積時間と直近の対象。
struct SynchronousTextureLoadStats {
    uint64_t attempts = 0;
    uint64_t successes = 0;
    uint64_t failures = 0;
    double totalMilliseconds = 0.0;
    double lastMilliseconds = 0.0;
    double maxMilliseconds = 0.0;
    std::string lastPath;
};

/// @note Byte storage is frozen while any queued renderer consumer owns the batch.
struct ConstantBufferUpdateEntry { ResourceHandle<ConstantBufferTag> handle; size_t offset = 0, size = 0; };
struct ConstantBufferUpdateBatch { std::vector<uint8_t> bytes; std::vector<ConstantBufferUpdateEntry> entries; };

class ResourceManager {
public:
    FBZZ_GRAPHICS_API explicit ResourceManager(IRenderer& renderer);
    FBZZ_GRAPHICS_API ~ResourceManager();
    /// @note Application より長く生存する Manager から renderer の参照を切る終了境界。以後の GPU 操作は禁止。
    FBZZ_GRAPHICS_API void PrepareRendererShutdown();
    /// @note 描画状態は Manager ごとに独立し、Reset で GPU 実体とともに破棄する。
    FBZZ_GRAPHICS_API RenderResources& Rendering();
    FBZZ_GRAPHICS_API void ReleaseRenderView(uint32_t key);
    /// @note ホストの描画フレーム開始時に一度呼ぶ。Play のリセットでは巻き戻さない。
    void AdvanceFrame() { ++m_frameStamp; }
    [[nodiscard]] uint64_t FrameStamp() const { return m_frameStamp; }
    /// @note Application が初期化時に登録する唯一のインスタンス。
    /// @note System から ResourceManager& を受け取れない場面で使うフォールバック。
    FBZZ_GRAPHICS_API static ResourceManager* Active();
    /// @return 作成元が終了済みなら nullptr。呼出しは資源管理スレッドに限る。
    [[nodiscard]] FBZZ_GRAPHICS_API static ResourceManager* FindOwner(uint64_t owner);

    FBZZ_GRAPHICS_API ResourceHandle<ShaderTag> LoadShader(std::string_view path);
    /// @return Manager が所有するマゼンタ材質。バックエンドが診断シェーダーを生成できなければ nullptr。
    /// @note 外部アセットを読まない。Reset 後も次回取得で再生成する。呼び手は内容を変更しない。
    FBZZ_GRAPHICS_API Material* GetErrorMaterial(bool skinned);
    /// @note 成功した shader load / reload と同じ版の能力を返す。失敗した reload は旧能力を保持する。
    [[nodiscard]] FBZZ_GRAPHICS_API ShaderCapabilities GetShaderCapabilities(ResourceHandle<ShaderTag> handle) const;

    /// @note レンダースレッドのフレーム外で呼ぶ。失敗時は旧実体とハンドルを保持する。
    FBZZ_GRAPHICS_API ResourceHandle<ShaderTag> ReloadShader(std::string_view path);

    /// @note 全候補を生成してから一括で差し替える。失敗時は旧状態を保持して false。
    /// @note レンダースレッドのフレーム外で呼ぶ。
    FBZZ_GRAPHICS_API bool ReloadAllShaders();
    [[nodiscard]] uint64_t GetShaderVersion() const { return m_shaderVersion; }
    /// @note デバイスロスト復帰用に全 GPU リソースを破棄し、次フレームの遅延再生成を促す。
    /// @note 旧 D3D デバイスに紐づく COM リソースは新デバイスで再利用できないため。
    FBZZ_GRAPHICS_API void Reset();
    [[nodiscard]] uint64_t GetResetVersion() const { return m_resetVersion; }
    /// @note プロセス内で単調に割り当てる所有者 ID。アドレス再利用と Reset 世代を区別する。
    [[nodiscard]] uint64_t GetInstanceId() const { return m_instanceId; }
    /// @brief flipGreen は法線画像の G を GPU 転送前に反転し、生画像とは別にキャッシュする。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> LoadTexture(std::string_view path, bool flipGreen = false);
    /// @note 同名ファイルを上書きした際、既存ハンドルを維持したまま GPU テクスチャを差し替える。
    /// @note IBL ベイク後もパスキャッシュが古い DDS を返し続けるため、明示的な再ロードが必要。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> ReloadTexture(std::string_view path, bool flipGreen = false);

    /// @note path (またはそのフォルダ配下) のテクスチャをキャッシュから外し、GPU 実体を解放する。
    /// @note LoadTexture はパスキャッシュに当たった時点で即返すため、ファイルを消しても
    /// @note       エディタを再起動するまで古い絵が出続ける。消したつもりのアセットが配布物へ
    /// @note       持ち込まれるのを防ぐために必要。
    /// @note 既に配られたハンドルは無効になる。参照側は次のフレームで解決し直す前提
    /// @note       (UIImage / MaterialComponent は loadedTexturePath を見て再解決する)。
    /// @param path Assets/ 起点の相対パス。フォルダを渡すと配下をまとめて外す。
    /// @return 外したエントリ数。
    FBZZ_GRAPHICS_API std::size_t EvictTexture(std::string_view path);
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> CreateTexture(const uint8_t* rgba, uint32_t width, uint32_t height,
                                             Where where = Where::current());
    /// @note CPU で焼いたミップ連鎖 (RGBA8) からテクスチャを作る。
    /// @param mips 0 段目から順。空、または未対応バックエンドでは無効ハンドルを返す。
    /// @note 縮小を CPU 側で決めたいテクスチャ (水面のさざ波勾配タイルなど) 用。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> CreateTextureWithMips(const TextureMipData* mips, uint32_t mipCount,
                                                     Where where = Where::current());

    /// @name 非同期テクスチャ転送
    /// @see Docs/design/asset-streaming.md «GPU 転送と公開»
    /// @{
    /// @brief CreateTextureWithMips の待たない版。返したハンドルはパスキャッシュに載らない。
    /// @param outUploadToken IsUploadComplete に渡す値。0 は完了済み。
    /// @note 完了前のハンドルを描画へ渡さない。公開は完了確認後に PublishTexture で行う。
    /// @note 完了前に Release してよい。GPU 実体の返却はバックエンドのフェンス管理が遅らせる。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> BeginTextureUpload(const TextureMipData* mips, uint32_t mipCount,
                                                  uint64_t& outUploadToken,
                                                  Where where = Where::current());
    /// @brief BeginTextureUpload の転送が GPU 上で終わったか。CPU は待たない。
    [[nodiscard]] FBZZ_GRAPHICS_API bool IsUploadComplete(uint64_t uploadToken) const;
    /// @brief パスキャッシュに載っているテクスチャを引く。読み込みはしない。
    [[nodiscard]] FBZZ_GRAPHICS_API ResourceHandle<TextureTag> FindCachedTexture(std::string_view path,
                                                                bool flipGreen = false) const;
    /// @brief 転送を終えたテクスチャを path のキャッシュへ載せ、以後の LoadTexture が同じ実体を返すようにする。
    /// @return キャッシュに載ったハンドル。既に同じ path が載っていれば既存を返し、uploaded は返却する。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> PublishTexture(std::string_view path,
                                              ResourceHandle<TextureTag> uploaded,
                                              bool flipGreen = false);
    /// @brief uploaded の実体を target のスロットへ移し、target の旧実体を返す (品質変更の差し替え)。
    /// @note target のハンドルは有効なまま中身だけが変わる。旧実体の GPU 返却と bindless 枠の回収は
    /// @note       バックエンドのフェンス管理が遅らせるので、記録済みの描画が読み終えるまで生きる。
    /// @return 移せなければ false (uploaded はそのまま)。
    FBZZ_GRAPHICS_API bool ReplaceTextureContents(ResourceHandle<TextureTag> target, ResourceHandle<TextureTag> uploaded);
    /// @brief LoadTexture と同じ読み込みとキャッシュだが、«外部に配った» 印を付けない。
    /// @note 非同期経路 (TextureAsset) だけが使う。印の無い実体は EvictStreamedTexture で外せる。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> LoadTextureUnpinned(std::string_view path, bool flipGreen = false);
    /// @brief 非同期経路が載せたテクスチャをキャッシュから外して返す。
    /// @return LoadTexture で配られたことがある (誰かがハンドルを持っているかもしれない) なら false で何もしない。
    FBZZ_GRAPHICS_API bool EvictStreamedTexture(std::string_view path, bool flipGreen = false);
    /// @brief テクスチャ 1 枚の概算バイト数 (RGBA8 換算)。無効なハンドルは 0。
    [[nodiscard]] FBZZ_GRAPHICS_API std::size_t GetTextureBytes(ResourceHandle<TextureTag> handle) const;
    /// @}
    /// @note テクスチャ未設定のフォールバックが共有する 1x1 白テクスチャ。
    /// @note 呼ぶたびに CreateTexture すると «誰も返さない 1 枚» が実体ごとに増える。
    /// @note       パーティクル / トレイルは寿命が短く数も多いので、そのぶんだけ漏れ続ける。
    [[nodiscard]] FBZZ_GRAPHICS_API ResourceHandle<TextureTag> GetWhiteTexture();
    /// @brief 同期テクスチャ読み込みの計測値を返す。
    /// @note LoadTexture 系のキャッシュミスを計測する。ResourceManager のレンダースレッドから読む。
    [[nodiscard]] const SynchronousTextureLoadStats& GetSynchronousTextureLoadStats() const
    {
        return m_syncTextureLoadStats;
    }
    /// @note CPU生成したRGBA8ボリュームから3D Textureを作る。Color LUTなどに使用する。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> CreateTexture3D(
        const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth,
        Where where = Where::current());

    FBZZ_GRAPHICS_API ResourceHandle<BufferTag> CreateVertexBuffer(const void* data, size_t bytes, uint32_t stride,
                                                 Where where = Where::current());
    /// @note CS が書き込み、IA が頂点として読むバッファ (コンピュートスキニングの出力先)。
    /// @note 未対応バックエンドでは無効ハンドルが返る。呼び出し側は VS スキニングへフォールバックすること。
    FBZZ_GRAPHICS_API ResourceHandle<BufferTag> CreateGpuWritableVertexBuffer(size_t bytes, uint32_t stride,
                                                            Where where = Where::current());
    FBZZ_GRAPHICS_API ResourceHandle<BufferTag> CreateIndexBuffer(const void* data, uint32_t count,
                                                Where where = Where::current());
    FBZZ_GRAPHICS_API ResourceHandle<ConstantBufferTag> CreateConstantBuffer(size_t sizeBytes,
                                                           Where where = Where::current());
    FBZZ_GRAPHICS_API ResourceHandle<PipelineStateTag> CreatePipelineState(const PipelineStateDesc& desc,
                                                          Where where = Where::current());
    /// @note 形式と深度の有無を明示して生成する。
    /// @note colorCount = 0 は深度専用。withDepth = false かつ colorCount = 0 は作れない。
    FBZZ_GRAPHICS_API ResourceHandle<RenderTargetTag> CreateRenderTarget(uint32_t width, uint32_t height,
                                                       const RenderTargetDesc& desc,
                                                       Where where = Where::current());
    /// @note 従来どおり RGBA16F + 深度付きで生成する短縮形。
    FBZZ_GRAPHICS_API ResourceHandle<RenderTargetTag> CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount = 1,
                                                       Where where = Where::current());
    /// @note 6 面キューブマップ描画先を生成する (空連動 IBL の SkyCapture 用)。
    /// @note 面の描画は IRenderer::SetRenderTargetFace、サンプリングは GetCubemapTexture() で取得した
    /// @note TextureTag を DrawCall.textures[] へ束縛して行う。未対応バックエンドでは Null を返す。
    FBZZ_GRAPHICS_API ResourceHandle<RenderTargetTag> CreateCubemapRenderTarget(uint32_t size, uint32_t mipCount = 1,
                                                              Where where = Where::current());
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> CreateComputeTexture(uint32_t width, uint32_t height,
                                                    Where where = Where::current());
    /// @note CS が RWTexture3D として書き、後段が Texture3D として読むボリューム (フロクセル霧)。
    /// @note 未対応バックエンドでは Null ハンドルが返る。呼び出し側は機能ごと落とすこと。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> CreateComputeTexture3D(
        uint32_t width, uint32_t height, uint32_t depth,
        Where where = Where::current());
    /// @note CPU から矩形単位で書き換えられるテクスチャ (ゼロクリア済み) を作る。
    /// @note 更新は Get(handle)->UpdateRegion(...) で行う。フォントの動的アトラスが使う。
    /// @note 未対応バックエンドでは Null ハンドルが返る。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> CreateDynamicTexture(
        uint32_t width, uint32_t height, DynamicTextureFormat format,
        Where where = Where::current());
    /// @note 外部 (IRenderer) が生成済みの ITexture を ResourcePool に引き取り TextureTag を返す。
    /// @note 空連動 IBL の BakeSkyLight が畳み込んだキューブ SRV ラッパーを Lit パスへ束縛可能にする。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> RegisterTexture(std::unique_ptr<ITexture> texture,
                                               Where where = Where::current());
    /// @note GPU Instancing 用 StructuredBuffer。elementCount 個・stride バイトの DYNAMIC バッファを作成する。
    FBZZ_GRAPHICS_API ResourceHandle<StructuredBufferTag> CreateStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride,
                                                               Where where = Where::current());
    /// @note 初期データを一度だけ転送し、GPU ローカルな読み取り専用 SRV として保持する。
    /// @note 毎フレーム不変なスキニング入力を UPLOAD Heap から読むと PCIe/UMA 経路が律速になるため。
    FBZZ_GRAPHICS_API ResourceHandle<StructuredBufferTag> CreateGpuLocalStructuredBuffer(
        const void* data, uint32_t elementCount, uint32_t stride,
        Where where = Where::current());
    /// @note CS が RWStructuredBuffer として書き込む DEFAULT バッファ (SRV + UAV)。GPU パーティクルプール等に使う。
    FBZZ_GRAPHICS_API ResourceHandle<StructuredBufferTag> CreateRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride,
                                                                 Where where = Where::current());

    FBZZ_GRAPHICS_API IShader* Get(ResourceHandle<ShaderTag> h);
    FBZZ_GRAPHICS_API ITexture* Get(ResourceHandle<TextureTag> h);
    FBZZ_GRAPHICS_API IBuffer* Get(ResourceHandle<BufferTag> h);
    FBZZ_GRAPHICS_API IConstantBuffer* Get(ResourceHandle<ConstantBufferTag> h);
    FBZZ_GRAPHICS_API IPipelineState* Get(ResourceHandle<PipelineStateTag> h);
    FBZZ_GRAPHICS_API IRenderTarget* Get(ResourceHandle<RenderTargetTag> h);
    FBZZ_GRAPHICS_API IStructuredBuffer* Get(ResourceHandle<StructuredBufferTag> h);
    FBZZ_GRAPHICS_API IAccelerationStructure* Get(ResourceHandle<AccelerationStructureTag> h);

    /// @brief Resolves adjacent native buffer reads after a single CPU-frame drain.
    /// @pre Both spans have the same size and remain valid during wait-pump callbacks.
    /// @note Invalid/stale handles receive empty states; an entirely invalid input does not wait.
    /// @note Values belong to this read only; resource mutation or creation requires another read before reuse.
    FBZZ_GRAPHICS_API void GetBufferStates(std::span<const ResourceHandle<BufferTag>> handles,
                         std::span<BufferResourceState> out);

    /// @brief Reads mirrored buffer metadata without resolving a native resource or waiting for the CPU frame.
    /// @param handle Generation-checked buffer handle.
    /// @param out Receives the metadata only when the handle is live.
    /// @return False for stale or unknown handles; out remains unchanged.
    [[nodiscard]] FBZZ_GRAPHICS_API bool GetBufferMetadata(ResourceHandle<BufferTag> handle,
                                         BufferResourceMetadata& out) const;

    /// @brief Returns the manager-owned static meshlet geometry cache.
    [[nodiscard]] FBZZ_GRAPHICS_API MeshletGeometryCache& Meshlets();

    /// @return Null on unsupported hardware or invalid geometry; creation does not record GPU work.
    FBZZ_GRAPHICS_API ResourceHandle<AccelerationStructureTag> CreateAccelerationStructure(
        const AccelerationStructureDesc& desc, Where where = Where::current());

    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> GetColorTexture(ResourceHandle<RenderTargetTag> rt, uint32_t index = 0);
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> GetDepthTexture(ResourceHandle<RenderTargetTag> rt);
    /// @note キューブマップ RT の TextureCube SRV ハンドル (CreateCubemapRenderTarget で作った RT 用)。
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> GetCubemapTexture(ResourceHandle<RenderTargetTag> rt);

    FBZZ_GRAPHICS_API void Update(ResourceHandle<BufferTag> h, const void* data, size_t sizeBytes);
    FBZZ_GRAPHICS_API void Update(ResourceHandle<ConstantBufferTag> h, const void* data, size_t sizeBytes);
    /// @note The existing executor owns immutable bytes; no byte copy or new RHI operation is introduced.
    FBZZ_GRAPHICS_API void UpdateConstantBuffers(std::shared_ptr<const ConstantBufferUpdateBatch> batch);
    FBZZ_GRAPHICS_API bool DefersResourceUpdates() const;
    FBZZ_GRAPHICS_API void Update(ResourceHandle<StructuredBufferTag> h, const void* data, size_t sizeBytes);
    [[nodiscard]] FBZZ_GRAPHICS_API bool UpdateRegion(ResourceHandle<StructuredBufferTag> h, const void* data, size_t sizeBytes, size_t offsetBytes);
    FBZZ_GRAPHICS_API void Release(ResourceHandle<ShaderTag> h);
    FBZZ_GRAPHICS_API void Release(ResourceHandle<TextureTag> h);
    FBZZ_GRAPHICS_API void Release(ResourceHandle<BufferTag> h);
    FBZZ_GRAPHICS_API void Release(ResourceHandle<ConstantBufferTag> h);
    FBZZ_GRAPHICS_API void Release(ResourceHandle<PipelineStateTag> h);
    FBZZ_GRAPHICS_API void Release(ResourceHandle<RenderTargetTag> h);
    FBZZ_GRAPHICS_API void Release(ResourceHandle<StructuredBufferTag> h);
    FBZZ_GRAPHICS_API void Release(ResourceHandle<AccelerationStructureTag> h);

    /// @note 生存中のリソース追跡情報を全プールぶん out へ追記する。
    /// @note 台帳は疎な固定長配列なので、索引で 1 件ずつ取る API は先頭から数え直すことになる。
    /// @note       全件を舐める用途 (一覧・差分・終了時の集計) しか無いので、それを許すと必ず
    /// @note       本数の 2 乗のループが書かれる。
    FBZZ_GRAPHICS_API void CollectLiveDebugResources(std::vector<core::AllocationInfo>& out) const;
    /// @note 生存中リソースの申告バイト合計 (O(1))。
    [[nodiscard]] FBZZ_GRAPHICS_API std::size_t GetLiveDebugResourceBytes() const;
    /// @note 詳細台帳を列挙せず、生存中の追跡件数を全プールから集計する (O(1))。
    [[nodiscard]] FBZZ_GRAPHICS_API std::size_t GetLiveDebugResourceCount() const;

    /// @note フレームごとの増加を見張り、増え続けたら発生位置つきで警告する。フレームに 1 回呼ぶ。
    /// @note 「毎フレーム少しずつ増える」類はパネルを開いて見比べない限り気付けないため、
    /// @note       自動で見張る (1 分遊べば数百 MB になるものを «気付いた人だけが直す» 形にしない)。
    /// @note 検証構成 (FBZZ_GPU_VALIDATION) でだけ動く。Release では何もしない。
    FBZZ_GRAPHICS_API void TickLeakWatchdog();

    /// @note Mesh の頂点 / インデックスバッファを返し、ハンドルを空にする。
    /// @return 返した本数。
    /// @note Mesh は «形» を持つだけの構造体で ResourceManager を知らない。持たせると、
    /// @note       マネージャーより長生きする静的キャッシュ上の Mesh が終了後に返しにいく。
    /// @note       所有者 (Model / ProceduralMesh などの Component) が畳まれる場所で明示的に呼ぶ。
    FBZZ_GRAPHICS_API std::size_t ReleaseMeshBuffers(Mesh& mesh);

private:
    friend struct prepared_skinning_detail::TicketFactory;
    friend class InstanceBatcher;
    friend class Material;
    friend class material_transfer_detail::MaterialTransferPayloadScope;
    friend class buffer_state_detail::BufferStateReadScope;
    /// @note Wait on the attached facade before using native data or a proof that replaces its read.
    void SynchronizeBufferAccess(bool nativeRead);
    FBZZ_GRAPHICS_API void ReadBufferStates(std::span<const ResourceHandle<BufferTag>> handles,
                          std::span<BufferResourceState> out,
                          buffer_state_detail::BufferStateReadScope* scope);
    /// @note Re-resolves each generation between native getters because custom RHI getters can call back into resource mutation.
    FBZZ_GRAPHICS_API void ReadNativeBufferStates(std::span<const ResourceHandle<BufferTag>> handles,
                                std::span<BufferResourceState> out);
    /// @note Called only after exact registered-facade proof; recheck the facade and producer for each Material upload.
    [[nodiscard]] FBZZ_GRAPHICS_API bool IsMaterialTransferProducer(const IRenderer* expectedFacade) const;
    /// @note Each destination keeps its ordered update; the shared owner contains no GPU or manager reference.
    FBZZ_GRAPHICS_API void UpdateOwnedMaterialBytes(ResourceHandle<ConstantBufferTag> handle,
        std::shared_ptr<const material_transfer_detail::OwnedConstantBufferData> payload);
    std::unique_ptr<RenderResources> m_renderResources;
    uint64_t m_frameStamp = 0;
    FBZZ_GRAPHICS_API static uint64_t Key(ResourceHandle<RenderTargetTag> h);
    FBZZ_GRAPHICS_API ResourceHandle<TextureTag> LoadTextureImpl(std::string_view path, bool pin, bool flipGreen);
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateTextureForPath(const std::string& sourcePath, bool flipGreen);
    FBZZ_GRAPHICS_API void ReleaseOwnedResourcesForShutdown();
    FBZZ_GRAPHICS_API void LogLiveDebugResources() const;
    FBZZ_GRAPHICS_API void RegisterBufferMetadata(ResourceHandle<BufferTag> handle, size_t sizeBytes,
                                uint32_t stride, bool cpuReadable,
                                bool gpuWritable, bool indexBuffer);
    [[nodiscard]] FBZZ_GRAPHICS_API bool AdvanceBufferContentVersion(ResourceHandle<BufferTag> handle,
                                                   size_t updateSizeBytes);
    FBZZ_GRAPHICS_API void RemoveBufferMetadata(ResourceHandle<BufferTag> handle);

    IRenderer& m_renderer;

    ResourcePool<IShader, ShaderTag> m_shaders;
    ResourcePool<ITexture, TextureTag> m_textures;
    ResourcePool<IBuffer, BufferTag> m_buffers;
    ResourcePool<IConstantBuffer, ConstantBufferTag> m_constantBuffers;
    ResourcePool<IPipelineState, PipelineStateTag> m_pipelineStates;
    ResourcePool<IRenderTarget, RenderTargetTag> m_renderTargets;
    ResourcePool<IStructuredBuffer, StructuredBufferTag> m_structuredBuffers;
    ResourcePool<IAccelerationStructure, AccelerationStructureTag> m_accelerationStructures;

    mutable std::mutex m_bufferMetadataMutex;
    std::vector<BufferResourceMetadata> m_bufferMetadata = std::vector<BufferResourceMetadata>(1);
    std::unique_ptr<MeshletGeometryCache> m_meshletGeometryCache;

    std::unordered_map<std::string, ResourceHandle<ShaderTag>> m_shaderCache;
    std::unordered_map<uint64_t, ShaderCapabilities> m_shaderCapabilities;
    std::unordered_map<std::string, ResourceHandle<TextureTag>> m_textureCache;
    SynchronousTextureLoadStats m_syncTextureLoadStats;
    /// @note LoadTexture で外へ配ったキー。配られた先がハンドルを持ち続けるかもしれないので、非同期側は外さない。
    std::unordered_set<std::string> m_texturesHandedOut;
    std::unordered_map<uint64_t, std::vector<ResourceHandle<TextureTag>>> m_renderTargetColors;
    std::unordered_map<uint64_t, ResourceHandle<TextureTag>> m_renderTargetDepths;
    ResourceHandle<TextureTag> m_whiteTexture;
    std::unique_ptr<Material> m_surfaceErrorMaterial;
    std::unique_ptr<Material> m_skinnedErrorMaterial;
    uint64_t m_resetVersion = 1;
    uint64_t m_instanceId = 0;
    bool m_rendererDetached = false;
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
    FBZZ_GRAPHICS_API bool Ensure(ResourceManager& resources, uint32_t width, uint32_t height, uint32_t colorCount = 1,
                Where where = Where::current());
    /// @brief 形式・深度の有無・深度の向きまで指定する版。desc が変わっても作り直す。
    FBZZ_GRAPHICS_API bool Ensure(ResourceManager& resources, uint32_t width, uint32_t height, const RenderTargetDesc& desc,
                Where where = Where::current());
    /// @note 明示的に返す。以後の Ensure は作り直しから始まる。
    FBZZ_GRAPHICS_API void Release(ResourceManager& resources);

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
