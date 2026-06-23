// FBZZ Engine
// DX11IblBaker.hpp | fbzz::renderer
// IIblBaker の DX11 実装
//
// DX11 の Compute Shader を直接発行して IBL テクスチャを GPU ベイクし、
// DirectXTex の CaptureTexture + SaveToDDSFile で DDS ファイルに書き出す。
//
// 設計上の注意:
//   - ResourceManager / IRenderer は一切使わない (Texture2DArray UAV 操作が必要なため)
//   - DX11 デバイスは DX11Renderer::GetDevice()/GetDeviceContext() から受け取る
//   - このクラスは DX11 プラットフォーム層内に閉じており、上位からは IIblBaker* で触れる
#pragma once
#include <Engine/Renderer/IIblBaker.hpp>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace fbzz::renderer {

class DX11IblBaker final : public IIblBaker {
public:
    DX11IblBaker(ID3D11Device* device, ID3D11DeviceContext* context);

    [[nodiscard]] bool Bake(
        const IblBakeInput& input,
        const std::string&  outputDir,
        const std::string&  baseName,
        IblBakeOutput&      output) override;

private:
    // ── CB レイアウト (HLSL 側と厳密に一致させる) ──────────────────────────

    // EquirectToCubemap / IrradianceConvolution 共通
    struct alignas(16) CbIblFace {
        uint32_t faceIndex;
        uint32_t textureSize;
        uint32_t _pad[2];
    };

    // PrefilteredEnvMap 専用
    struct alignas(16) CbPrefilter {
        uint32_t faceIndex;
        uint32_t outputSize;
        float    roughness;
        uint32_t sampleCount;
        uint32_t envMipCount;
        uint32_t _pad[3];
    };

    // ── GPU リソース生成ヘルパー ─────────────────────────────────────────────

    // Equirectangular float ピクセルを GPU Texture2D (R32G32B32A32_FLOAT) にアップロード
    Microsoft::WRL::ComPtr<ID3D11Texture2D> UploadEquirect(
        const float* pixels, uint32_t w, uint32_t h);

    // 6 面 Cubemap 用 Texture2DArray を作成 (SRV + 面×mip の UAV を後で個別作成)
    // bindUav: CS 出力先として使う場合 true (D3D11_BIND_UNORDERED_ACCESS 追加)
    // generateMips: 入力環境 Cubemap 用。RTV と GenerateMips フラグを追加する。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateCubemapTexture(
        uint32_t size, uint32_t mipCount, DXGI_FORMAT format, bool bindUav,
        bool generateMips = false);

    // 指定 face・mip の Texture2DArray スライスに対する UAV を作成する
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> CreateFaceUAV(
        ID3D11Texture2D* tex, uint32_t face, uint32_t mip = 0);

    // ── ベイクステージ ───────────────────────────────────────────────────────

    // Equirect SRV → Environment Cubemap (6 面 Dispatch × 1)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> BakeEnvCubemap(
        ID3D11ShaderResourceView* equirectSrv, uint32_t size);

    // Env SRV → Irradiance Cubemap (6 面 Dispatch × 1)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> BakeIrradiance(
        ID3D11ShaderResourceView* envSrv, uint32_t size);

    // Env SRV → Prefiltered Cubemap (6 面 × mipCount 回 Dispatch)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> BakePrefiltered(
        ID3D11ShaderResourceView* envSrv,
        uint32_t size, uint32_t mipCount,
        uint32_t sampleCount, uint32_t envMipCount);

    // 既存 BRDFIntegration.cs.cso で BRDF LUT を生成 (1 回 Dispatch)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> BakeBrdfLut(uint32_t size);

    // ── DDS 保存 ─────────────────────────────────────────────────────────────

    // Cubemap Texture2DArray を DDS に保存 (DirectXTex CaptureTexture → SaveToDDSFile)
    bool SaveToDDS(ID3D11Texture2D* tex, const std::string& absPath);

    // ── 共通ユーティリティ ────────────────────────────────────────────────────

    // CSO ファイルをバイト列に読み込む
    static std::vector<uint8_t> LoadBinary(const std::string& path);

    // 定数バッファを更新する (Map/Unmap)
    template<typename T>
    void UpdateCB(ID3D11Buffer* cb, const T& data);

    // 即時 CB を作成する (D3D11_USAGE_DYNAMIC)
    Microsoft::WRL::ComPtr<ID3D11Buffer> CreateCB(size_t sizeBytes);

    ID3D11Device*        m_device;
    ID3D11DeviceContext* m_context;

    // ── Compute Shader ──────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csEquirect;     // EquirectToCubemap
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csIrradiance;   // IrradianceConvolution
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csPrefilter;    // PrefilteredEnvMap
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csBrdfLut;      // BRDFIntegration (既存)

    // ── サンプラー ──────────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D11SamplerState>  m_samplerLinearWrap;
};

} // namespace fbzz::renderer
