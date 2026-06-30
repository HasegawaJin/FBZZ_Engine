// FBZZ Engine
// DX11IblBaker.cpp | fbzz::renderer
// IIblBaker の DX11 実装 — GPU Compute でベイク → DirectXTex で DDS 保存
//
// 処理フロー:
//   1. CSO を読み込み ComputeShader を生成
//   2. Equirect float* → GPU Texture2D (R32G32B32A32_FLOAT)
//   3. EquirectToCubemap CS × 6 面 → Environment Cubemap (R16G16B16A16_FLOAT)
//   4. IrradianceConvolution CS × 6 面 → Irradiance Cubemap (R16G16B16A16_FLOAT)
//   5. PrefilteredEnvMap CS × (face × mip) → Prefiltered Cubemap (R16G16B16A16_FLOAT)
//   6. BRDFIntegration CS × 1 → BRDF LUT (R16G16B16A16_FLOAT、RG を使用)
//   7. CaptureTexture → SaveToDDSFile × 4
//   8. FzIblHeader を .ibl に書き出す
#pragma comment(lib, "ole32.lib") // DirectXTex が内部で WIC を使う

#include "DX11IblBaker.hpp"
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <DirectXTex.h>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace fbzz::renderer {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// 構築
// ─────────────────────────────────────────────────────────────────────────────

DX11IblBaker::DX11IblBaker(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device), m_context(context)
{
}

// ─────────────────────────────────────────────────────────────────────────────
// メインエントリー
// ─────────────────────────────────────────────────────────────────────────────

bool DX11IblBaker::BakeCore(const IblBakeInput& input, BakedCubemaps& out)
{
    if (!input.pixels || input.equirectW == 0 || input.equirectH == 0) {
        FBZZ_LOG_ERROR("DX11IblBaker: ピクセルデータが空です");
        return false;
    }
    if (input.compiledShadersDir.empty()) {
        FBZZ_LOG_ERROR("DX11IblBaker: compiledShadersDir が未設定です");
        return false;
    }

    // ── Shader 読み込み ───────────────────────────────────────────────────
    const std::string& dir = input.compiledShadersDir;
    auto LoadCS = [&](const std::string& name, ComPtr<ID3D11ComputeShader>& cs) -> bool {
        auto blob = LoadBinary(dir + name);
        if (blob.empty()) return false;
        HRESULT hr = m_device->CreateComputeShader(
            blob.data(), blob.size(), nullptr, cs.GetAddressOf());
        if (FAILED(hr)) {
            FBZZ_LOG_ERROR("DX11IblBaker: CS 生成失敗 [%s] hr=0x%08X", name.c_str(), (unsigned)hr);
            return false;
        }
        return true;
    };

    if (!LoadCS("IBL.EquirectToCubemap.cs.cso",     m_csEquirect))   return false;
    if (!LoadCS("IBL.IrradianceConvolution.cs.cso", m_csIrradiance)) return false;
    if (!LoadCS("IBL.PrefilteredEnvMap.cs.cso",     m_csPrefilter))  return false;
    if (!LoadCS("PostProcess.AmbientOcclusion.BRDFIntegration.cs.cso", m_csBrdfLut)) return false;

    // ── サンプラー (LinearWrap) ───────────────────────────────────────────
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        m_device->CreateSamplerState(&sd, m_samplerLinearWrap.GetAddressOf());
    }

    // ── Step 1: Equirect → GPU ───────────────────────────────────────────
    auto equirectTex = UploadEquirect(input.pixels, input.equirectW, input.equirectH);
    if (!equirectTex) return false;

    ComPtr<ID3D11ShaderResourceView> equirectSrv;
    m_device->CreateShaderResourceView(equirectTex.Get(), nullptr, equirectSrv.GetAddressOf());

    // ── Step 2: Environment Cubemap ───────────────────────────────────────
    out.env = BakeEnvCubemap(equirectSrv.Get(), input.envCubemapSize);
    if (!out.env) return false;

    D3D11_TEXTURE2D_DESC envDesc{};
    out.env->GetDesc(&envDesc);
    out.envMipCount = envDesc.MipLevels;

    // Env Cubemap SRV (TextureCube)
    out.envSrv = CreateCubeSRV(out.env.Get(), envDesc.MipLevels);
    if (!out.envSrv) return false;

    // WHAT: mip0 に書き込んだ HDR 環境を全 mip に縮小し、prefilter の PDF ベース LOD に使う。
    // WHY: 高輝度の太陽ピクセルを常に mip0 から確率的に拾うと、粗い鏡面に白い firefly が残る。
    m_context->GenerateMips(out.envSrv.Get());

    // ── Step 3: Irradiance ────────────────────────────────────────────────
    out.irradiance = BakeIrradiance(out.envSrv.Get(), input.irradianceSize);
    if (!out.irradiance) return false;

    // ── Step 4: Prefiltered ───────────────────────────────────────────────
    out.prefiltered = BakePrefiltered(
        out.envSrv.Get(),
        input.prefilteredSize,
        input.prefilteredMipCount,
        input.sampleCount,
        envDesc.MipLevels);
    if (!out.prefiltered) return false;

    // ── Step 5: BRDF LUT ──────────────────────────────────────────────────
    out.brdfLut = BakeBrdfLut(input.brdfLutSize);
    if (!out.brdfLut) return false;

    return true;
}

ComPtr<ID3D11ShaderResourceView> DX11IblBaker::CreateCubeSRV(
    ID3D11Texture2D* tex, uint32_t mipLevels)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format                        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.ViewDimension                 = D3D11_SRV_DIMENSION_TEXTURECUBE;
    desc.TextureCube.MostDetailedMip   = 0;
    desc.TextureCube.MipLevels         = mipLevels;

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(m_device->CreateShaderResourceView(tex, &desc, srv.GetAddressOf()))) {
        FBZZ_LOG_ERROR("DX11IblBaker: Cubemap SRV 生成失敗 (mipLevels=%u)", mipLevels);
        return nullptr;
    }
    return srv;
}

IblTextureSet DX11IblBaker::BakeToTextures(const IblBakeInput& input)
{
    IblTextureSet result; // 失敗時は IsValid()==false のまま返す

    BakedCubemaps baked;
    if (!BakeCore(input, baked)) return result;

    result.envCube             = baked.env;
    result.envCubeSrv          = baked.envSrv; // BakeCore が GenerateMips 済みの TextureCube SRV
    result.irradiance          = baked.irradiance;
    result.prefiltered         = baked.prefiltered;
    result.prefilteredMipCount = input.prefilteredMipCount;

    // irradiance(1 mip) / prefilter(prefilteredMipCount) の TextureCube SRV を付与する。
    result.irradianceSrv  = CreateCubeSRV(baked.irradiance.Get(), 1);
    result.prefilteredSrv = CreateCubeSRV(baked.prefiltered.Get(), input.prefilteredMipCount);
    if (!result.irradianceSrv || !result.prefilteredSrv)
        return IblTextureSet{}; // SRV 生成失敗 → 空 set

    return result;
}

bool DX11IblBaker::EnsureConvolutionResources(const std::string& compiledShadersDir)
{
    auto LoadCS = [&](const std::string& name, ComPtr<ID3D11ComputeShader>& cs) -> bool {
        if (cs) return true; // ロード済み
        auto blob = LoadBinary(compiledShadersDir + name);
        if (blob.empty()) return false;
        if (FAILED(m_device->CreateComputeShader(blob.data(), blob.size(), nullptr, cs.GetAddressOf()))) {
            FBZZ_LOG_ERROR("DX11IblBaker: 畳み込み CS 生成失敗 [%s]", name.c_str());
            return false;
        }
        return true;
    };

    if (!LoadCS("IBL.IrradianceConvolution.cs.cso", m_csIrradiance)) return false;
    if (!LoadCS("IBL.PrefilteredEnvMap.cs.cso",     m_csPrefilter))  return false;

    if (!m_samplerLinearWrap) {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        m_device->CreateSamplerState(&sd, m_samplerLinearWrap.GetAddressOf());
    }
    return true;
}

IblTextureSet DX11IblBaker::ConvolveCubeToTextures(
    ID3D11ShaderResourceView* envCubeSRV,
    const std::string&        compiledShadersDir,
    uint32_t irradianceSize, uint32_t prefilteredSize,
    uint32_t prefilteredMipCount, uint32_t sampleCount, uint32_t envMipCount)
{
    IblTextureSet result; // 失敗時は IsValid()==false

    if (!envCubeSRV) return result;
    if (!EnsureConvolutionResources(compiledShadersDir)) return result;

    // 既存の実証済み畳み込みステージを、外部から渡された環境キューブ SRV に対して走らせる。
    auto irrTex = BakeIrradiance(envCubeSRV, irradianceSize);
    if (!irrTex) return result;
    auto preTex = BakePrefiltered(envCubeSRV, prefilteredSize, prefilteredMipCount, sampleCount, envMipCount);
    if (!preTex) return result;

    result.irradiance          = irrTex;
    result.prefiltered         = preTex;
    result.prefilteredMipCount = prefilteredMipCount;
    result.irradianceSrv       = CreateCubeSRV(irrTex.Get(), 1);
    result.prefilteredSrv      = CreateCubeSRV(preTex.Get(), prefilteredMipCount);
    if (!result.irradianceSrv || !result.prefilteredSrv)
        return IblTextureSet{};

    return result;
}

bool DX11IblBaker::Bake(
    const IblBakeInput& input,
    const std::string&  outputDir,
    const std::string&  baseName,
    IblBakeOutput&      output)
{
    // 出力ディレクトリ作成 (Compute コア実行前に用意する)
    std::error_code ec;
    fs::create_directories(util::StringUtils::ToWide(outputDir), ec);

    // ── Compute コア (Bake / BakeToTextures 共通) ─────────────────────────
    BakedCubemaps baked;
    if (!BakeCore(input, baked)) return false;

    // ── DDS 保存 ──────────────────────────────────────────────────────────
    auto makePath = [&](const char* suffix) {
        return outputDir + "/" + baseName + suffix + ".dds";
    };

    const std::string envPath      = makePath("_env");
    const std::string irrPath      = makePath("_irr");
    const std::string prefilterPath = makePath("_prefilter");
    const std::string brdfPath     = makePath("_brdf");

    if (!SaveToDDS(baked.env.Get(),         envPath))       return false;
    if (!SaveToDDS(baked.irradiance.Get(),  irrPath))       return false;
    if (!SaveToDDS(baked.prefiltered.Get(), prefilterPath)) return false;
    if (!SaveToDDS(baked.brdfLut.Get(),     brdfPath))      return false;

    output.envCubemapPath      = envPath;
    output.irradiancePath      = irrPath;
    output.prefilteredPath     = prefilterPath;
    output.brdfLutPath         = brdfPath;
    output.prefilteredMipCount = input.prefilteredMipCount;

    // ── Step 7: .ibl バイナリ記述子を書き出す ────────────────────────────
    // IblImporter がこのファイルを読み込んで各 DDS のパスを解決する。
    // DDS パスは .ibl と同一ディレクトリからの相対パス (ファイル名のみ) で格納する。
    {
        asset::FzIblHeader header{};
        std::memcpy(header.magic, "FZIBL\0", 6);
        header.version             = asset::FZIBL_VERSION;
        header.prefilteredMipCount = input.prefilteredMipCount;
        header._pad                = 0;

        const std::string envRel       = baseName + "_env.dds";
        const std::string irrRel       = baseName + "_irr.dds";
        const std::string prefilterRel = baseName + "_prefilter.dds";
        const std::string brdfRel      = baseName + "_brdf.dds";

        std::strncpy(header.envCubemapPath,  envRel.c_str(),       255);
        std::strncpy(header.irradiancePath,  irrRel.c_str(),       255);
        std::strncpy(header.prefilteredPath, prefilterRel.c_str(), 255);
        std::strncpy(header.brdfLutPath,     brdfRel.c_str(),      255);

        const std::string iblPath = outputDir + "/" + baseName + ".ibl";
        std::ofstream ofs(util::StringUtils::ToWide(iblPath), std::ios::binary);
        if (!ofs.is_open()) {
            FBZZ_LOG_ERROR("DX11IblBaker: .ibl ファイル書き込み失敗 [%s]", iblPath.c_str());
            return false;
        }
        ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU リソース生成
// ─────────────────────────────────────────────────────────────────────────────

ComPtr<ID3D11Texture2D> DX11IblBaker::UploadEquirect(
    const float* pixels, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width          = w;
    desc.Height         = h;
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc     = { 1, 0 };
    desc.Usage          = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem    = pixels;
    initData.SysMemPitch = w * 4u * sizeof(float); // RGBA float

    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = m_device->CreateTexture2D(&desc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11IblBaker: Equirect テクスチャ生成失敗 hr=0x%08X", (unsigned)hr);
        return nullptr;
    }
    return tex;
}

ComPtr<ID3D11Texture2D> DX11IblBaker::CreateCubemapTexture(
    uint32_t size, uint32_t mipCount, DXGI_FORMAT format, bool bindUav,
    bool generateMips)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width      = size;
    desc.Height     = size;
    desc.MipLevels  = mipCount;
    desc.ArraySize  = 6;
    desc.Format     = format;
    desc.SampleDesc = { 1, 0 };
    desc.Usage      = D3D11_USAGE_DEFAULT;
    desc.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    if (bindUav)
        desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    if (generateMips)
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    // TEXTURECUBE は SRV として TextureCube でサンプリングするために必要
    desc.MiscFlags  = D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (generateMips)
        desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11IblBaker: Cubemap Texture2D 生成失敗 hr=0x%08X size=%u mip=%u",
                       (unsigned)hr, size, mipCount);
        return nullptr;
    }
    return tex;
}

ComPtr<ID3D11UnorderedAccessView> DX11IblBaker::CreateFaceUAV(
    ID3D11Texture2D* tex, uint32_t face, uint32_t mip)
{
    // DX11 では Cubemap の各面は Texture2DArray のスライスとして UAV を作る
    D3D11_TEXTURE2D_DESC texDesc;
    tex->GetDesc(&texDesc);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format                           = texDesc.Format;
    uavDesc.ViewDimension                    = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    uavDesc.Texture2DArray.MipSlice          = mip;
    uavDesc.Texture2DArray.FirstArraySlice   = face;
    uavDesc.Texture2DArray.ArraySize         = 1;

    ComPtr<ID3D11UnorderedAccessView> uav;
    HRESULT hr = m_device->CreateUnorderedAccessView(tex, &uavDesc, uav.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11IblBaker: Face UAV 生成失敗 face=%u mip=%u hr=0x%08X",
                       face, mip, (unsigned)hr);
        return nullptr;
    }
    return uav;
}

// ─────────────────────────────────────────────────────────────────────────────
// ベイクステージ
// ─────────────────────────────────────────────────────────────────────────────

ComPtr<ID3D11Texture2D> DX11IblBaker::BakeEnvCubemap(
    ID3D11ShaderResourceView* equirectSrv, uint32_t size)
{
    // mip0 は EquirectToCubemap CS で生成し、残りは GenerateMips でローパス化する。
    uint32_t mipCount = 1;
    for (uint32_t extent = size; extent > 1; extent >>= 1)
        ++mipCount;

    auto tex = CreateCubemapTexture(
        size, mipCount, DXGI_FORMAT_R16G16B16A16_FLOAT, true, true);
    if (!tex) return nullptr;

    auto cb = CreateCB(sizeof(CbIblFace));
    if (!cb) return nullptr;

    const uint32_t groups = (size + 7u) / 8u;

    m_context->CSSetShader(m_csEquirect.Get(), nullptr, 0);
    m_context->CSSetShaderResources(0, 1, &equirectSrv);
    ID3D11SamplerState* samp = m_samplerLinearWrap.Get();
    m_context->CSSetSamplers(0, 1, &samp);
    m_context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());

    for (uint32_t face = 0; face < 6; ++face)
    {
        CbIblFace cbData{ face, size, 0, 0 };
        UpdateCB(cb.Get(), cbData);

        auto uav = CreateFaceUAV(tex.Get(), face, 0);
        if (!uav) return nullptr;

        ID3D11UnorderedAccessView* uavPtr = uav.Get();
        m_context->CSSetUnorderedAccessViews(0, 1, &uavPtr, nullptr);
        m_context->Dispatch(groups, groups, 1);
    }

    // CS バインドをクリア
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView*  nullSrv = nullptr;
    m_context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    m_context->CSSetShaderResources(0, 1, &nullSrv);
    m_context->CSSetShader(nullptr, nullptr, 0);

    return tex;
}

ComPtr<ID3D11Texture2D> DX11IblBaker::BakeIrradiance(
    ID3D11ShaderResourceView* envSrv, uint32_t size)
{
    auto tex = CreateCubemapTexture(size, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    if (!tex) return nullptr;

    auto cb = CreateCB(sizeof(CbIblFace));
    if (!cb) return nullptr;

    const uint32_t groups = (size + 7u) / 8u;

    m_context->CSSetShader(m_csIrradiance.Get(), nullptr, 0);
    m_context->CSSetShaderResources(0, 1, &envSrv);
    ID3D11SamplerState* samp = m_samplerLinearWrap.Get();
    m_context->CSSetSamplers(0, 1, &samp);
    m_context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());

    for (uint32_t face = 0; face < 6; ++face)
    {
        CbIblFace cbData{ face, size, 0, 0 };
        UpdateCB(cb.Get(), cbData);

        auto uav = CreateFaceUAV(tex.Get(), face, 0);
        if (!uav) return nullptr;

        ID3D11UnorderedAccessView* uavPtr = uav.Get();
        m_context->CSSetUnorderedAccessViews(0, 1, &uavPtr, nullptr);
        m_context->Dispatch(groups, groups, 1);
    }

    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView*  nullSrv = nullptr;
    m_context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    m_context->CSSetShaderResources(0, 1, &nullSrv);
    m_context->CSSetShader(nullptr, nullptr, 0);

    return tex;
}

ComPtr<ID3D11Texture2D> DX11IblBaker::BakePrefiltered(
    ID3D11ShaderResourceView* envSrv,
    uint32_t size, uint32_t mipCount,
    uint32_t sampleCount, uint32_t envMipCount)
{
    auto tex = CreateCubemapTexture(size, mipCount, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    if (!tex) return nullptr;

    auto cb = CreateCB(sizeof(CbPrefilter));
    if (!cb) return nullptr;

    m_context->CSSetShader(m_csPrefilter.Get(), nullptr, 0);
    m_context->CSSetShaderResources(0, 1, &envSrv);
    ID3D11SamplerState* samp = m_samplerLinearWrap.Get();
    m_context->CSSetSamplers(0, 1, &samp);
    m_context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());

    for (uint32_t mip = 0; mip < mipCount; ++mip)
    {
        const uint32_t mipSize = size >> mip;  // 512, 256, 128, ...
        const float    roughness = (mipCount > 1)
                                 ? static_cast<float>(mip) / static_cast<float>(mipCount - 1)
                                 : 0.0f;
        // (mipSize + 7) / 8 は mipSize >= 1 の前提で常に >= 1 になる
        const uint32_t groups = (mipSize + 7u) / 8u;

        for (uint32_t face = 0; face < 6; ++face)
        {
            CbPrefilter cbData;
            cbData.faceIndex   = face;
            cbData.outputSize  = mipSize;
            cbData.roughness   = roughness;
            cbData.sampleCount = sampleCount;
            cbData.envMipCount = envMipCount;
            cbData._pad[0] = cbData._pad[1] = cbData._pad[2] = 0;
            UpdateCB(cb.Get(), cbData);

            auto uav = CreateFaceUAV(tex.Get(), face, mip);
            if (!uav) return nullptr;

            ID3D11UnorderedAccessView* uavPtr = uav.Get();
            m_context->CSSetUnorderedAccessViews(0, 1, &uavPtr, nullptr);
            m_context->Dispatch(groups, groups, 1);
        }
    }

    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView*  nullSrv = nullptr;
    m_context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    m_context->CSSetShaderResources(0, 1, &nullSrv);
    m_context->CSSetShader(nullptr, nullptr, 0);

    return tex;
}

ComPtr<ID3D11Texture2D> DX11IblBaker::BakeBrdfLut(uint32_t size)
{
    // 実行時 ComputeTexture と同じ RGBA16F に統一し、BRDFIntegration.cs の typed UAV と一致させる。
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width      = size;
    desc.Height     = size;
    desc.MipLevels  = 1;
    desc.ArraySize  = 1;
    desc.Format     = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc = { 1, 0 };
    desc.Usage      = D3D11_USAGE_DEFAULT;
    desc.BindFlags  = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf()))) {
        FBZZ_LOG_ERROR("DX11IblBaker: BRDF LUT テクスチャ生成失敗");
        return nullptr;
    }

    ComPtr<ID3D11UnorderedAccessView> uav;
    if (FAILED(m_device->CreateUnorderedAccessView(tex.Get(), nullptr, uav.GetAddressOf()))) {
        FBZZ_LOG_ERROR("DX11IblBaker: BRDF LUT UAV 生成失敗");
        return nullptr;
    }

    // BRDFIntegration.cs は CB なし (GetDimensions でサイズを取得する)
    m_context->CSSetShader(m_csBrdfLut.Get(), nullptr, 0);

    ID3D11UnorderedAccessView* uavPtr = uav.Get();
    m_context->CSSetUnorderedAccessViews(0, 1, &uavPtr, nullptr);

    const uint32_t groups = (size + 7u) / 8u;
    m_context->Dispatch(groups, groups, 1);

    ID3D11UnorderedAccessView* nullUav = nullptr;
    m_context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    m_context->CSSetShader(nullptr, nullptr, 0);

    return tex;
}

// ─────────────────────────────────────────────────────────────────────────────
// DDS 保存
// ─────────────────────────────────────────────────────────────────────────────

bool DX11IblBaker::SaveToDDS(ID3D11Texture2D* tex, const std::string& absPath)
{
    // GPU → CPU へステージングコピー
    DirectX::ScratchImage image;
    HRESULT hr = DirectX::CaptureTexture(m_device, m_context, tex, image);
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11IblBaker: CaptureTexture 失敗 [%s] hr=0x%08X",
                       absPath.c_str(), (unsigned)hr);
        return false;
    }

    // DirectXTex の CaptureTexture は TEXTURECUBE フラグを引き継がない場合があるため
    // メタデータを手動で cubemap フラグに設定する
    DirectX::TexMetadata meta = image.GetMetadata();
    if (meta.arraySize == 6 && meta.dimension == DirectX::TEX_DIMENSION_TEXTURE2D)
        meta.miscFlags |= DirectX::TEX_MISC_TEXTURECUBE;

    const std::wstring wpath = util::StringUtils::ToWide(absPath);
    hr = DirectX::SaveToDDSFile(
        image.GetImages(), image.GetImageCount(), meta,
        DirectX::DDS_FLAGS_NONE, wpath.c_str());

    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11IblBaker: SaveToDDSFile 失敗 [%s] hr=0x%08X",
                       absPath.c_str(), (unsigned)hr);
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 共通ユーティリティ
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> DX11IblBaker::LoadBinary(const std::string& path)
{
    std::ifstream f(util::StringUtils::ToWide(path), std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        FBZZ_LOG_ERROR("DX11IblBaker: CSO 読み込み失敗 [%s]", path.c_str());
        return {};
    }
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

ComPtr<ID3D11Buffer> DX11IblBaker::CreateCB(size_t sizeBytes)
{
    // DX11 の制約: 定数バッファサイズは 16 バイト単位で切り上げる
    const size_t aligned = (sizeBytes + 15u) & ~15u;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = static_cast<UINT>(aligned);
    desc.Usage          = D3D11_USAGE_DYNAMIC;
    desc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Buffer> buf;
    HRESULT hr = m_device->CreateBuffer(&desc, nullptr, buf.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11IblBaker: 定数バッファ生成失敗 hr=0x%08X", (unsigned)hr);
        return nullptr;
    }
    return buf;
}

template<typename T>
void DX11IblBaker::UpdateCB(ID3D11Buffer* cb, const T& data)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(m_context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &data, sizeof(T));
        m_context->Unmap(cb, 0);
    }
}

} // namespace fbzz::renderer
