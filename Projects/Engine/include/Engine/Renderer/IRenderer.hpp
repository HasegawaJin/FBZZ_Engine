// FBZZ Engine
// IRenderer.hpp | fbzz::renderer
// Renderer バックエンドの抽象インターフェース
// 上位レイヤーは DX11 実装を直接参照せず、このインターフェースだけを使う。
// リソース生成は ResourceManager に閉じ、描画 API は Submit / Dispatch に集約する。
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "ComputeCall.hpp"
#include "DrawCall.hpp"
#include "IIblBaker.hpp"
#include "IBuffer.hpp"
#include "IConstantBuffer.hpp"
#include "IStructuredBuffer.hpp"
#include "IPipelineState.hpp"
#include "IRenderTarget.hpp"
#include "IShader.hpp"
#include "ITexture.hpp"
#include "RenderState.hpp"
#include "ResourceHandle.hpp"
#include "SamplerMode.hpp"
#include <Math/Vector4.hpp>

namespace fbzz::renderer {

class ResourceManager;

// GPU プロファイリング 1 パス分の結果。
// WHY: IRenderer を経由することで上位レイヤーが DX11Renderer を知らずに GPU 時間を取得できる。
struct GpuPassProfile {
    std::string name;
    double gpuMs = 0.0;
};

// RenderTarget 内部 SRV のどちらを TextureTag 化するかを表す。
// WHY: bool 引数では Color / Depth の意味が呼び出し側から読めず、誤指定に気づきにくいため。
enum class RenderTargetTextureKind : uint8_t {
    Color,
    Depth,
};

class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Clear(const math::Vector4& color) = 0;

    virtual void Submit(const DrawCall& call, ResourceManager& resources) = 0;
    virtual void Dispatch(const ComputeCall& call, ResourceManager& resources) = 0;

    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;

    virtual void SetRenderTarget(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources) = 0;
    virtual void ClearDepth(float depth = 1.0f) = 0;

    virtual void SetSampler(uint32_t slot, SamplerMode mode) = 0;

    // GPU プロファイリング。DX11Renderer のみ実装し、他バックエンドは no-op。
    // WHY: パスごとの GPU 実行時間を上位レイヤーから取得するために抽象化する。
    //      D3D11_QUERY_TIMESTAMP_DISJOINT / D3D11_QUERY_TIMESTAMP を使って非同期に計測する。
    //      GpuProfCollect() を呼んだ時点で QUERY_LATENCY フレーム前の結果が確定する。
    virtual void GpuProfBeginFrame()                      {}
    virtual void GpuProfEndFrame()                        {}
    virtual void GpuProfBeginPass(const char* /*name*/)   {}
    virtual void GpuProfEndPass(const char* /*name*/)     {}
    virtual void GpuProfCollect()                         {}
    virtual const std::vector<GpuPassProfile>& GpuProfGetResults() const
    {
        static const std::vector<GpuPassProfile> s_empty;
        return s_empty;
    }

    // IBL ベイク処理の実装を返す (Editor 専用)。
    // DX11Renderer は DX11IblBaker を返す。他のバックエンドは nullptr を返してよい。
    // WHY: IblBaker は DX11 固有の UAV 操作を必要とするため IRenderer の factory 経由で提供し、
    //      Editor が DX11Renderer に直接ダウンキャストしなくて済むようにする。
    virtual std::unique_ptr<IIblBaker> CreateIblBaker() { return nullptr; }

private:
    friend class ResourceManager;

    virtual std::unique_ptr<IBuffer> CreateNativeVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride) = 0;
    virtual std::unique_ptr<IBuffer> CreateNativeIndexBuffer(const void* data, uint32_t count) = 0;
    virtual std::unique_ptr<IConstantBuffer> CreateNativeConstantBuffer(size_t sizeBytes) = 0;
    virtual std::unique_ptr<IShader> CreateNativeShader(const std::string& path) = 0;
    virtual std::unique_ptr<ITexture> CreateNativeTexture(const std::string& path) = 0;
    virtual std::unique_ptr<ITexture> CreateNativeTextureFromData(const uint8_t* rgba, uint32_t width, uint32_t height) = 0;
    virtual std::unique_ptr<ITexture> CreateNativeTexture3DFromData(
        const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth) = 0;
    virtual std::unique_ptr<ITexture> CreateNativeTextureFromRenderTarget(
        IRenderTarget& rt,
        uint32_t index,
        RenderTargetTextureKind kind) = 0;
    virtual std::unique_ptr<IPipelineState> CreateNativePipelineState(const PipelineStateDesc& desc) = 0;
    virtual std::unique_ptr<IRenderTarget> CreateNativeRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount) = 0;
    virtual std::unique_ptr<ITexture> CreateNativeComputeTexture(uint32_t width, uint32_t height) = 0;
    virtual std::unique_ptr<IStructuredBuffer> CreateNativeStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride) = 0;
    virtual std::unique_ptr<IStructuredBuffer> CreateNativeRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride) = 0;
};

} // namespace fbzz::renderer
