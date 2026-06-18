// FBZZ Engine
// DX11Renderer.hpp | fbzz::renderer
// IRenderer の DX11 実装
// DX11 固有のデバイスオブジェクトと各種バインド処理を保持する。
// Application からは IRenderer として所有される。
//
// 設計方針:
//   上位レイヤー (Application / Sandbox) は IRenderer& のみを参照し、
//   このクラスに直接アクセスしない。依存方向: sandbox → engine (IRenderer) → DX11Renderer。
//   Application.cpp のみが DX11Renderer を make_unique して IRenderer に格納する
//   ファクトリー役を担い、それ以外の場所では DX11Renderer を知る必要がない。
//
//   サンプラー:
//     Init() 時に SamplerMode::COUNT 種のサンプラーを事前生成し、
//     SetSampler() で要求に応じてバインドする。
//     毎フレーム生成/破棄するのではなくキャッシュ方式を採用する。
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>

#include <Engine/Renderer/IRenderer.hpp>
#include "DX11Buffer.hpp"
#include "DX11RenderTarget.hpp"
#include "DX11StructuredBuffer.hpp"

namespace fbzz::renderer
{

class DX11Renderer : public IRenderer
{
public:
    // Win32 ウィンドウハンドルと初期解像度を受け取ってデバイス・スワップチェーンを構築する
    bool Init(HWND hwnd, std::uint32_t width, std::uint32_t height);

    // ClearState() でパイプラインをリセットしてから ComPtr が自動解放する
    void Shutdown();

    // OM に RTV + DSV をバインドし直す (SetRenderTarget() 後のフレーム先頭で呼ぶ)
    void BeginFrame() override;

    // Present the swap chain. Frame pacing is controlled by Time.
    void EndFrame() override;

    // RTV と DSV を指定色でクリアする (現在バインド中の RT に対して動作する)
    void Clear(const math::Vector4& color) override;

    // 現在バインド中の RT の深度バッファのみクリアする
    void ClearDepth(float depth = 1.0f) override;

    // DrawCall を受け取り、パイプラインステート → シェーダー → リソース → Draw の順で実行する
    void Submit(const DrawCall& call, ResourceManager& resources) override;

    // Compute Shader を Dispatch する (SSAO / Bloom 等のポストプロセス CS に使用)
    void Dispatch(const ComputeCall& call, ResourceManager& resources) override;

    // ウィンドウリサイズ時にスワップチェーン・RTV・DSV を再構築する
    void Resize(uint32_t width, uint32_t height) override;

    // オフスクリーン RT に切り替える (nullptr でバックバッファに戻す)
    void SetRenderTarget(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources) override;

    // スロット番号に対応するサンプラープリセットをバインドする
    void SetSampler(uint32_t slot, SamplerMode mode) override;

    // GPU プロファイリング (D3D11_QUERY_TIMESTAMP_DISJOINT / D3D11_QUERY_TIMESTAMP)
    // QUERY_LATENCY フレーム遅延のリングバッファ方式で非同期計測する。
    void GpuProfBeginFrame()                    override;
    void GpuProfEndFrame()                      override;
    void GpuProfBeginPass(const char* name)     override;
    void GpuProfEndPass(const char* name)       override;
    void GpuProfCollect()                       override;
    const std::vector<GpuPassProfile>& GpuProfGetResults() const override { return m_gpuResults; }

    uint32_t GetWidth()  const override { return m_width;  }
    uint32_t GetHeight() const override { return m_height; }

    // DX11Buffer 等の DX11 サブシステムが Init 時にデバイスを必要とする場合に使用
    ID3D11Device*        GetDevice()       const { return m_device.Get(); }
    ID3D11DeviceContext* GetDeviceContext() const { return m_context.Get(); }

private:
    std::unique_ptr<IBuffer>         CreateNativeVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride) override;
    std::unique_ptr<IBuffer>         CreateNativeIndexBuffer(const void* data, uint32_t count) override;
    std::unique_ptr<IConstantBuffer> CreateNativeConstantBuffer(size_t sizeBytes) override;
    std::unique_ptr<IShader>         CreateNativeShader(const std::string& path) override;
    std::unique_ptr<ITexture>        CreateNativeTexture(const std::string& path) override;
    std::unique_ptr<ITexture>        CreateNativeTextureFromData(const uint8_t* rgba, uint32_t width, uint32_t height) override;
    std::unique_ptr<IPipelineState>  CreateNativePipelineState(const PipelineStateDesc& desc) override;
    std::unique_ptr<IRenderTarget>   CreateNativeRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount) override;
    std::unique_ptr<ITexture>           CreateNativeComputeTexture(uint32_t width, uint32_t height) override;
    std::unique_ptr<IStructuredBuffer>  CreateNativeStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride) override;
    std::unique_ptr<IStructuredBuffer>  CreateNativeRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride) override;

    void BindRenderTarget(IRenderTarget* rt);

    // -------------------------------------------------------------------------
    // GPU Timestamp Query プール
    // -------------------------------------------------------------------------
    // WHY: D3D11 の GPU クエリ結果は発行した数フレーム後にしか CPU から読めない。
    //      QUERY_LATENCY フレーム分の Query オブジェクトをリングバッファで循環させ、
    //      毎フレーム GpuProfCollect() で古いフレームの結果を取り出す。
    static constexpr int GPU_QUERY_LATENCY = 3;
    static constexpr int GPU_MAX_PASSES    = 32;

    struct GpuQueryFrame {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> beginTs[GPU_MAX_PASSES];
        Microsoft::WRL::ComPtr<ID3D11Query> endTs[GPU_MAX_PASSES];
        char                                names[GPU_MAX_PASSES][64];
        int                                 count     = 0;
        bool                                begun     = false;
        bool                                ended     = false;
        // WHY: GetData が S_FALSE を返して収集をスキップしたまま書き込みインデックスが
        //      一周してきた場合、Begin() を呼ぶと D3D11 の ABANDONING_PREVIOUS_RESULTS
        //      警告が発生する。collected フラグで「まだ読んでいない」スロットへの
        //      上書きを防ぎ、そのフレームの GPU 計測を安全にスキップする。
        bool                                collected = true;
    };

    GpuQueryFrame            m_gpuFrames[GPU_QUERY_LATENCY];
    int                      m_gpuWriteIdx   = 0;  // 現在書き込んでいるフレームのインデックス
    int                      m_gpuCollectIdx = 0;  // 次に読み出すフレームのインデックス
    int                      m_gpuFilled     = 0;  // 書き終えたフレーム数 (latency に達するまで収集しない)
    std::vector<GpuPassProfile> m_gpuResults;

    // GPU Timestamp クエリを初期化する (BeginFrame の遅延初期化から呼ぶ)
    void InitGpuQueryFrame(GpuQueryFrame& frame);

    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain>         m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_depthStencilBuffer;

    // SamplerMode::COUNT 個のプリセットを Init 時に一括生成してキャッシュする
    Microsoft::WRL::ComPtr<ID3D11SamplerState>
        m_samplers[static_cast<int>(SamplerMode::COUNT)];

    // 現在バインド中のオフスクリーン RT (nullptr = バックバッファ)
    DX11RenderTarget* m_currentRT = nullptr;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    // --- Init 内部ヘルパー ---
    bool CreateRenderTargetView();
    bool CreateDepthStencilView();
    void InitSamplers();
};

} // namespace fbzz::renderer
