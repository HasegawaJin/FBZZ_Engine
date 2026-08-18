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

    // アクティブな描画バックエンドの表示名 ("DirectX 11" / "DirectX 12")。
    // WHY: ウィンドウタイトル等の UI 表示用。上位が具象型へダウンキャストせず
    //      (禁止事項) どちらのバックエンドで動作中かを判別できるようにする、純粋に情報提供的な API。
    virtual const char* GetBackendName() const { return "Unknown"; }

    // GPU バックエンドが保持するパイプライン参照を解除し、デバイス破棄前の状態を確定する。
    // WHY: ResourceManager がネイティブリソースを破棄しても、描画コンテキストにバインド中の
    //      リソースはバックエンド側の参照が残るため、デバイス破棄前に明示的な終了処理が必要。
    virtual void Shutdown() = 0;

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Clear(const math::Vector4& color) = 0;

    virtual void Submit(const DrawCall& call, ResourceManager& resources) = 0;
    virtual void Dispatch(const ComputeCall& call, ResourceManager& resources) = 0;
    // 相互依存しない Dispatch 群の UAV バリアをバッチ末尾へまとめる。
    // WHY: スキニングのように各 Dispatch が別バッファへ書くパスでは、Dispatch ごとの
    //      ResourceBarrier 呼び出しは不要。未対応バックエンドは既定の no-op でよい。
    virtual void BeginComputeBatch() {}
    virtual void EndComputeBatch() {}

    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;

    virtual void SetRenderTarget(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources) = 0;
    virtual void ClearDepth(float depth = 1.0f) = 0;

    // SetViewport — 現在の描画先の一部矩形だけへ描くようビューポートを絞る。
    // WHY: カスケードシャドウは 1 枚のシャドウマップを 2x2 のタイルに分け、
    //      カスケードごとに別のタイルへ描き込む (アトラス)。RT を分けずに済むので
    //      深度テクスチャは 1 本のまま、サンプル側も SRV 1 本で全カスケードを引ける。
    // NOTE: SetRenderTarget は必ずビューポートを RT 全体へ戻すため、
    //       「SetRenderTarget → SetViewport」の順で呼ぶこと。
    //       絞ったビューポートは次の SetRenderTarget まで有効。
    virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;

    // SetRenderTargetFace — キューブマップ RT の 1 面 (+mip) を描画先にバインドする。
    // WHY: 空連動 IBL の SkyCapture が、空ドームを 6 面それぞれの向きで描き込むために使う。
    //      DX11 以外のバックエンドは未対応 (no-op)。CreateCubemapRenderTarget で作った RT 以外を
    //      渡した場合の挙動は実装依存 (DX11 は何もしない)。
    virtual void SetRenderTargetFace(ResourceHandle<RenderTargetTag> /*rt*/, uint32_t /*face*/,
                                     uint32_t /*mip*/, ResourceManager& /*resources*/) {}

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

    // BakeSkyLight — キャプチャ済み空キューブマップ (envCubeRT) を irradiance / prefilter キューブへ
    // 畳み込み、それぞれを ITexture (TextureCube SRV) として返す。空連動 IBL の runtime 畳み込み経路。
    // WHY: 畳み込み Compute は DX11 固有 (面ごとの Texture2DArray UAV) のため、抽象 IRenderer の入口だけ
    //      提供し実装は DX11 に閉じる。返した ITexture は呼び出し側が ResourceManager::RegisterTexture で所有する。
    //      DX11 以外のバックエンドは未対応 (false)。
    virtual bool BakeSkyLight(ResourceHandle<RenderTargetTag> /*envCubeRT*/, ResourceManager& /*resources*/,
                              uint32_t /*irradianceSize*/, uint32_t /*prefilterSize*/,
                              uint32_t /*prefilterMips*/, uint32_t /*sampleCount*/,
                              std::unique_ptr<ITexture>& /*outIrradiance*/,
                              std::unique_ptr<ITexture>& /*outPrefilter*/) { return false; }

    // IBL ベイク処理の実装を返す (Editor 専用)。
    // DX11Renderer は DX11IblBaker を返す。他のバックエンドは nullptr を返してよい。
    // WHY: IblBaker は DX11 固有の UAV 操作を必要とするため IRenderer の factory 経由で提供し、
    //      Editor が DX11Renderer に直接ダウンキャストしなくて済むようにする。
    virtual std::unique_ptr<IIblBaker> CreateIblBaker() { return nullptr; }

    // CaptureRenderTargetToPng — 指定 RenderTarget のカラーを PNG バイト列として CPU へ読み戻す。
    // WHY: AI 連携 (MCP viewport.capture) が「現在の Scene View を Claude に見せる」ために使う。
    //      GPU テクスチャ読み戻し・PNG エンコードはバックエンド固有 (DX11: CopyResource + DirectXTex /
    //      DX12: CommandQueue + DirectXTex) のため、上位 Editor が具象へダウンキャストせずに済むよう
    //      抽象入口だけ提供し、実装は各 Platform に閉じる。DX11/DX12 が実装し、他は未対応 (false)。
    //      outPng は PNG ファイル全体のバイト列、out{Width,Height} は実 RT サイズ (要求サイズではない)。
    virtual bool CaptureRenderTargetToPng(ResourceHandle<RenderTargetTag> /*rt*/, ResourceManager& /*resources*/,
                                          std::vector<uint8_t>& /*outPng*/,
                                          uint32_t& /*outWidth*/, uint32_t& /*outHeight*/) { return false; }

    // CaptureRenderTargetToLinearRGBA — 同じ RT を「絵」ではなく「数値」として読み戻す。
    // WHY: PNG は 8bit UNORM へクランプされるため、白飛びしているのか単に明るいのかが
    //      エンコードの時点で失われる。露出・画面占有・動きの量を機械的に判定するには
    //      HDR のままの線形値が要る (AI の VFX 評価 = vfx.previewMetrics が使う)。
    //      outRgba は width*height*4 の行優先 float 列。トーンマップもガンマ変換も行わない。
    virtual bool CaptureRenderTargetToLinearRGBA(ResourceHandle<RenderTargetTag> /*rt*/, ResourceManager& /*resources*/,
                                                 std::vector<float>& /*outRgba*/,
                                                 uint32_t& /*outWidth*/, uint32_t& /*outHeight*/) { return false; }

private:
    friend class ResourceManager;

    virtual std::unique_ptr<IBuffer> CreateNativeVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride) = 0;
    // CS が書き込み、IA が頂点として読むバッファ。コンピュートスキニングの出力先。
    // WHY: スキニング結果を 1 度だけ計算してシャドウ・GBuffer・Forward で共有するため、
    //      同じバッファに UAV 書き込みと頂点入力の両方を許す必要がある。
    //      未対応バックエンドは nullptr を返してよい (呼び出し側は VS スキニングへフォールバックする)。
    virtual std::unique_ptr<IBuffer> CreateNativeGpuWritableVertexBuffer(size_t /*sizeBytes*/, uint32_t /*stride*/) { return nullptr; }
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
    // 6 面キューブマップ描画先。未対応バックエンドは nullptr を返してよい (DX11 のみ実装)。
    virtual std::unique_ptr<IRenderTarget> CreateNativeCubemapRenderTarget(uint32_t /*size*/, uint32_t /*mipCount*/) { return nullptr; }
    // キューブマップ RT の TextureCube SRV を ITexture 化する (TextureTag として束縛可能にする)。
    virtual std::unique_ptr<ITexture> CreateNativeCubeTextureFromRenderTarget(IRenderTarget& /*rt*/) { return nullptr; }
    virtual std::unique_ptr<ITexture> CreateNativeComputeTexture(uint32_t width, uint32_t height) = 0;
    // CPU から矩形単位で書き換えられるテクスチャ。ITexture::UpdateRegion と対で使う。
    // 中身は未初期化ではなくゼロクリアされた状態で返すこと。
    //
    // WHY: フォントの動的アトラス (使われたグリフだけを実行時にラスタライズして貼る) が要求する。
    //      Immutable な CreateNativeTextureFromData では 1 グリフ増えるたびに
    //      テクスチャ全体を作り直すことになり、ハンドルも毎回変わってしまう。
    //      未対応バックエンドは nullptr を返してよい (呼び出し側は静的アトラスへ縮退する)。
    virtual std::unique_ptr<ITexture> CreateNativeDynamicTexture(
        uint32_t /*width*/, uint32_t /*height*/, DynamicTextureFormat /*format*/) { return nullptr; }
    virtual std::unique_ptr<IStructuredBuffer> CreateNativeStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride) = 0;
    // 初期データだけを持つ GPU ローカル SRV。DX11 など専用経路がない場合は通常の SRV へ縮退する。
    virtual std::unique_ptr<IStructuredBuffer> CreateNativeGpuLocalStructuredBuffer(
        const void* data, uint32_t elementCount, uint32_t stride)
    {
        return CreateNativeStructuredBuffer(data, elementCount, stride);
    }
    virtual std::unique_ptr<IStructuredBuffer> CreateNativeRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride) = 0;
};

} // namespace fbzz::renderer
