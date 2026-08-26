// FBZZ Engine
// ResourceManager.hpp | fbzz::renderer
// Renderer リソースの所有とハンドル解決
// IRenderer の非公開生成 API を呼べる唯一の窓口。
// 上位システムは shared_ptr ではなく ResourceHandle を保持する。
#pragma once
#include <Engine/Core/Memory/AllocationInfo.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ResourcePool.hpp>
#include <Engine/Renderer/RenderState.hpp>
// WHY: DynamicTextureFormat を CreateDynamicTexture の引数に取るため、
//      ITexture の前方宣言だけでは足りず実体が要る (ヘッダ自体は cstdint のみに依存する軽量なもの)。
#include <Engine/Renderer/ITexture.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
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

class ResourceManager {
public:
    explicit ResourceManager(IRenderer& renderer);
    ~ResourceManager();
    // Application が初期化時に登録する唯一のインスタンス。
    // System から ResourceManager& を受け取れない場面で使うフォールバック。
    static ResourceManager* Active();

    ResourceHandle<ShaderTag> LoadShader(std::string_view path);

    // HLSL ホットリロード: 既にロード済みのシェーダーを CSO ファイルから再読み込みする。
    // WHY: シェーダーを参照するすべての Material / PipelineState を更新せずに済むよう、
    //      ResourcePool::Replace で同じハンドルのスロット内容だけを差し替える。
    //      未ロードのパスは LoadShader() にフォールスルーする。
    ResourceHandle<ShaderTag> ReloadShader(std::string_view path);

    // HLSL ホットリロード: キャッシュ済みシェーダーをすべて再読み込みする。
    // WHY: HLSL変更時にcompile_shaders.ps1が影響を受けたシェーダーを再コンパイルするため、
    //      個別パスではなく一括で呼ぶ方が効率的。
    void ReloadAllShaders();
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
    ResourceHandle<TextureTag> CreateTexture(const uint8_t* rgba, uint32_t width, uint32_t height);
    // CPU生成したRGBA8ボリュームから3D Textureを作る。Color LUTなどに使用する。
    ResourceHandle<TextureTag> CreateTexture3D(
        const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth);

    ResourceHandle<BufferTag> CreateVertexBuffer(const void* data, size_t bytes, uint32_t stride);
    // CS が書き込み、IA が頂点として読むバッファ (コンピュートスキニングの出力先)。
    // 未対応バックエンドでは無効ハンドルが返る。呼び出し側は VS スキニングへフォールバックすること。
    ResourceHandle<BufferTag> CreateGpuWritableVertexBuffer(size_t bytes, uint32_t stride);
    ResourceHandle<BufferTag> CreateIndexBuffer(const void* data, uint32_t count);
    ResourceHandle<ConstantBufferTag> CreateConstantBuffer(size_t sizeBytes);
    ResourceHandle<PipelineStateTag> CreatePipelineState(const PipelineStateDesc& desc);
    ResourceHandle<RenderTargetTag> CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount = 1);
    // 6 面キューブマップ描画先を生成する (空連動 IBL の SkyCapture 用)。
    // 面の描画は IRenderer::SetRenderTargetFace、サンプリングは GetCubemapTexture() で取得した
    // TextureTag を DrawCall.textures[] へ束縛して行う。未対応バックエンドでは Null を返す。
    ResourceHandle<RenderTargetTag> CreateCubemapRenderTarget(uint32_t size, uint32_t mipCount = 1);
    ResourceHandle<TextureTag> CreateComputeTexture(uint32_t width, uint32_t height);
    // CS が RWTexture3D として書き、後段が Texture3D として読むボリューム (フロクセル霧)。
    // 未対応バックエンドでは Null ハンドルが返る。呼び出し側は機能ごと落とすこと。
    ResourceHandle<TextureTag> CreateComputeTexture3D(
        uint32_t width, uint32_t height, uint32_t depth);
    // CPU から矩形単位で書き換えられるテクスチャ (ゼロクリア済み) を作る。
    // 更新は Get(handle)->UpdateRegion(...) で行う。フォントの動的アトラスが使う。
    // 未対応バックエンドでは Null ハンドルが返る。
    ResourceHandle<TextureTag> CreateDynamicTexture(
        uint32_t width, uint32_t height, DynamicTextureFormat format);
    // 外部 (IRenderer) が生成済みの ITexture を ResourcePool に引き取り TextureTag を返す。
    // WHY: 空連動 IBL の BakeSkyLight が畳み込んだキューブ SRV ラッパーを Lit パスへ束縛可能にする。
    ResourceHandle<TextureTag> RegisterTexture(std::unique_ptr<ITexture> texture);
    // GPU Instancing 用 StructuredBuffer。elementCount 個・stride バイトの DYNAMIC バッファを作成する。
    ResourceHandle<StructuredBufferTag> CreateStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride);
    // 初期データを一度だけ転送し、GPU ローカルな読み取り専用 SRV として保持する。
    // WHY: 毎フレーム不変なスキニング入力を UPLOAD Heap から読むと PCIe/UMA 経路が律速になるため。
    ResourceHandle<StructuredBufferTag> CreateGpuLocalStructuredBuffer(
        const void* data, uint32_t elementCount, uint32_t stride);
    // CS が RWStructuredBuffer として書き込む DEFAULT バッファ (SRV + UAV)。GPU パーティクルプール等に使う。
    ResourceHandle<StructuredBufferTag> CreateRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride);

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
    [[nodiscard]] const core::AllocationInfo* GetLiveDebugResource(std::size_t index) const;

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
    uint64_t m_resetVersion = 1;
};

} // namespace fbzz::renderer
