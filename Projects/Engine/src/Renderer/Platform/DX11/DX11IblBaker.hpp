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

// IblTextureSet — BakeToTextures() の結果。GPU 常駐の IBL キューブマップ群 (DDS を経由しない)。
// WHY: 空連動 IBL では数フレームごとにベイクし直すため、DDS への書き出し/再ロードを避け、
//      GPU テクスチャと TextureCube SRV をそのまま保持して各 Lit パスへ供給する。
// 注意: 本構造体は ID3D11* を含むため DX11 プラットフォーム層に閉じている。上位の抽象 SkyLightBake
//       パスへ渡す際は別途 ResourceManager 登録経路を設ける (環境システム設計 §6 Phase A 続き)。
struct IblTextureSet {
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          envCube;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> envCubeSrv;       // TextureCube SRV (全 mip)
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          irradiance;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> irradianceSrv;    // TextureCube SRV
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          prefiltered;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prefilteredSrv;   // TextureCube SRV (全 mip)
    uint32_t                                         prefilteredMipCount = 0;

    bool IsValid() const { return irradianceSrv && prefilteredSrv; }
};

class DX11IblBaker final : public IIblBaker {
public:
    DX11IblBaker(ID3D11Device* device, ID3D11DeviceContext* context);

    [[nodiscard]] bool Bake(
        const IblBakeInput& input,
        const std::string&  outputDir,
        const std::string&  baseName,
        IblBakeOutput&      output) override;

    // BakeToTextures — Bake() と同一の Compute コアで IBL を焼き、DDS を書かず
    // GPU 常駐テクスチャ (+TextureCube SRV) として返す実行時ベイク経路。
    // WHY: 空連動 IBL のように毎フレーム/数フレームごとに焼き直す用途で、ファイル I/O を避ける。
    //      Bake() と BakeToTextures() は BakeCore() を共有し、出力先 (DDS / GPU テクスチャ) だけが異なる。
    // 失敗時は IsValid()==false の空 set を返す。
    [[nodiscard]] IblTextureSet BakeToTextures(const IblBakeInput& input);

    // ConvolveCubeToTextures — 既存の環境キューブ SRV (例: 実行時にキャプチャした空) を直接
    // irradiance / prefilter キューブへ畳み込む。Equirect アップロード/環境生成を経由しない runtime 経路。
    // compiledShadersDir: 畳み込み CSO のあるディレクトリ (末尾 "/")。畳み込み CS は初回に遅延ロードする。
    // envMipCount: 入力キューブの mip 数 (prefilter の env LOD 参照用。mip0 のみなら 1)。
    // 失敗時は IsValid()==false の空 set を返す。
    [[nodiscard]] IblTextureSet ConvolveCubeToTextures(
        ID3D11ShaderResourceView* envCubeSRV,
        const std::string&        compiledShadersDir,
        uint32_t irradianceSize, uint32_t prefilteredSize,
        uint32_t prefilteredMipCount, uint32_t sampleCount, uint32_t envMipCount);

private:
    // 畳み込み CS (irradiance / prefilter) とサンプラーを遅延ロードする (runtime 経路用)。
    bool EnsureConvolutionResources(const std::string& compiledShadersDir);

    // BakedCubemaps — BakeCore() が生成する中間 GPU テクスチャ群。
    // Bake() はこれを DDS 保存し、BakeToTextures() は SRV を付けて返す。
    struct BakedCubemaps {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          env;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> envSrv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          irradiance;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          prefiltered;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          brdfLut;
        uint32_t                                         envMipCount = 0;
    };

    // BakeCore — 入力検証 → Equirect アップロード → Env / Irradiance / Prefilter / BRDF の
    // 全 Compute ステージを実行する共通コア。DDS 保存と GPU 直書きの両経路が共有する。
    [[nodiscard]] bool BakeCore(const IblBakeInput& input, BakedCubemaps& out);

    // 焼いたキューブマップ Texture2DArray に対する TextureCube SRV を作成する。
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateCubeSRV(
        ID3D11Texture2D* tex, uint32_t mipLevels);


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
