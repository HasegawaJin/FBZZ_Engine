// FBZZ Engine
// DX12PsoCache.cpp | fbzz::renderer
// 固定スロット Root Signature と描画状態別 PSO の遅延構築
#include "DX12PsoCache.hpp"

#include "DX12Context.hpp"
#include "DX12Shader.hpp"
#include <Engine/Core/Logger.hpp>
#include <array>
#include <d3dcompiler.h>

namespace fbzz::renderer {

namespace {

// 静的サンプラーはシェーダーレジスタ (s0〜) 単位で決まる。Assets/Shaders/Common/Binding.hlsli の
// SAMPLER_* 定義と 1:1 で対応させること。
//
// WHY: DX11 は SetSampler(slot, mode) でパスごとに差し替えられるが、DX12 は Root Signature に
//      焼き込む静的サンプラーなので、レジスタごとに 1 つの意味へ固定するしかない。以前は
//      SamplerMode の列挙順をそのままレジスタ番号として並べていたため、s1 が比較サンプラーでなく
//      通常 Linear (ComparisonFunc=NEVER) に、s4 が wrap でなく clamp になっていた。
//      前者は SampleCmpLevelZero が常に 0 を返して全面影に、後者はタイラブルな 3D ノイズが
//      端テクセルへ張り付いて雲が一枚の白い板になる。
std::array<D3D12_STATIC_SAMPLER_DESC, 9> MakeStaticSamplers()
{
    struct Preset {
        D3D12_FILTER filter;
        D3D12_TEXTURE_ADDRESS_MODE address;
        UINT maxAnisotropy;
        D3D12_COMPARISON_FUNC comparison;
    };
    // s6〜s8 は現状どのシェーダーも宣言していない予約枠。
    constexpr Preset kPresets[9] = {
        // s0 SAMPLER_DEFAULT      : メッシュテクスチャのタイリングが主用途
        { D3D12_FILTER_ANISOTROPIC,                       D3D12_TEXTURE_ADDRESS_MODE_WRAP,   16, D3D12_COMPARISON_FUNC_NEVER },
        // s1 SAMPLER_SHADOW       : SamplerComparisonState (PCF)
        { D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 1, D3D12_COMPARISON_FUNC_LESS_EQUAL },
        // s2 SAMPLER_LINEAR_CLAMP : IBL BRDF LUT / 3D LUT / スプラットマップ
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR,                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   1, D3D12_COMPARISON_FUNC_NEVER },
        // s3 SAMPLER_POINT_CLAMP  : TAA 再投影ルックアップ
        { D3D12_FILTER_MIN_MAG_MIP_POINT,                 D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   1, D3D12_COMPARISON_FUNC_NEVER },
        // s4 SAMPLER_WRAP_LINEAR  : ボリューメトリック雲のタイラブル 3D ノイズ
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR,                D3D12_TEXTURE_ADDRESS_MODE_WRAP,    1, D3D12_COMPARISON_FUNC_NEVER },
        // s5                      : UI スプライト / テキスト
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR,                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   1, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_POINT,                 D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   1, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR,                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   1, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_ANISOTROPIC,                       D3D12_TEXTURE_ADDRESS_MODE_WRAP,    4, D3D12_COMPARISON_FUNC_NEVER },
    };

    std::array<D3D12_STATIC_SAMPLER_DESC, 9> samplers{};
    for (UINT slot = 0; slot < samplers.size(); ++slot) {
        auto& sampler = samplers[slot];
        sampler.Filter = kPresets[slot].filter;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = kPresets[slot].address;
        sampler.MaxAnisotropy = kPresets[slot].maxAnisotropy;
        sampler.ComparisonFunc = kPresets[slot].comparison;
        // ライト錐台外のシャドウサンプルは「照らされている」に倒す。
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = slot;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    return samplers;
}

} // namespace

size_t DX12PsoCache::KeyHash::operator()(const Key& key) const
{
    size_t hash = std::hash<const DX12Shader*>{}(key.shader);
    hash ^= static_cast<size_t>(key.rasterizer) << 3;
    hash ^= static_cast<size_t>(key.blend) << 7;
    hash ^= static_cast<size_t>(key.depth) << 11;
    hash ^= static_cast<size_t>(key.topology) << 15;
    hash ^= static_cast<size_t>(key.renderTargetFormat) << 19;
    hash ^= static_cast<size_t>(key.renderTargetCount) << 25;
    return hash;
}

bool DX12PsoCache::Initialize(ID3D12Device* device)
{
    if (!device) return false;
    m_device = device;
    return CreateRootSignature() && CreateComputeRootSignature();
}

void DX12PsoCache::Shutdown()
{
    m_cache.clear();
    m_computeCache.clear();
    m_computeRootSignature.Reset();
    m_rootSignature.Reset();
    m_device.Reset();
}

bool DX12PsoCache::CreateComputeRootSignature()
{
    std::array<D3D12_ROOT_PARAMETER1, 16> parameters{};
    for (UINT slot = 0; slot < 14; ++slot) {
        parameters[slot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[slot].Descriptor.ShaderRegister = slot;
        parameters[slot].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameters[slot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_DESCRIPTOR_RANGE1 srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 32;
    srvRange.BaseShaderRegister = 0;
    srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    parameters[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[14].DescriptorTable = {1, &srvRange};
    D3D12_DESCRIPTOR_RANGE1 uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 8;
    uavRange.BaseShaderRegister = 0;
    uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    parameters[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[15].DescriptorTable = {1, &uavRange};
    const auto samplers = MakeStaticSamplers();
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    desc.Desc_1_1.pParameters = parameters.data();
    desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    desc.Desc_1_1.pStaticSamplers = samplers.data();
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &errors))) {
        const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
        FBZZ_LOG_ERROR("DX12PsoCache: Compute Root Signature生成失敗: %s", message);
        return false;
    }
    return SUCCEEDED(m_device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSignature)));
}

bool DX12PsoCache::CreateRootSignature()
{
    std::array<D3D12_ROOT_PARAMETER1, 16> parameters{};
    for (UINT slot = 0; slot < 14; ++slot) {
        parameters[slot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[slot].Descriptor.ShaderRegister = slot;
        parameters[slot].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameters[slot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_DESCRIPTOR_RANGE1 pixelSrv{};
    pixelSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    pixelSrv.NumDescriptors = 32;
    pixelSrv.BaseShaderRegister = 0;
    pixelSrv.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    pixelSrv.OffsetInDescriptorsFromTableStart = 0;
    parameters[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[14].DescriptorTable = {1, &pixelSrv};
    parameters[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE1 vertexSrv{};
    vertexSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    vertexSrv.NumDescriptors = 16;
    vertexSrv.BaseShaderRegister = 0;
    vertexSrv.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    vertexSrv.OffsetInDescriptorsFromTableStart = 0;
    parameters[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[15].DescriptorTable = {1, &vertexSrv};
    parameters[15].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    const auto samplers = MakeStaticSamplers();

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    desc.Desc_1_1.pParameters = parameters.data();
    desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    desc.Desc_1_1.pStaticSamplers = samplers.data();
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3D12SerializeVersionedRootSignature(&desc, &blob, &errors);
    if (FAILED(result)) {
        const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
        FBZZ_LOG_ERROR("DX12PsoCache: Root Signature の生成に失敗しました: %s", message);
        return false;
    }
    return SUCCEEDED(m_device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
}

ID3D12PipelineState* DX12PsoCache::GetOrCreate(
    const DX12Shader& shader, const PipelineStateDesc& state, PrimitiveTopology topology,
    DXGI_FORMAT renderTargetFormat, uint32_t renderTargetCount)
{
    const Key key{&shader, state.rasterizer, state.blend, state.depth, topology,
                  renderTargetFormat, renderTargetCount};
    if (const auto found = m_cache.find(key); found != m_cache.end())
        return found->second.Get();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS = shader.GetVertexBytecode();
    desc.PS = shader.GetPixelBytecode();
    const auto& input = shader.GetInputLayout();
    desc.InputLayout = {input.data(), static_cast<UINT>(input.size())};
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = 1;
    desc.NumRenderTargets = renderTargetCount;
    desc.BlendState.IndependentBlendEnable = renderTargetCount > 1;
    for (uint32_t index = 0; index < renderTargetCount; ++index)
        desc.RTVFormats[index] = renderTargetFormat;
    desc.DSVFormat = DX12Context::DEPTH_FORMAT;
    desc.PrimitiveTopologyType = topology == PrimitiveTopology::LINE_LIST
        ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    desc.RasterizerState.FillMode = state.rasterizer == RasterizerMode::WIREFRAME
        ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = state.rasterizer == RasterizerMode::SOLID_NOCULL
        ? D3D12_CULL_MODE_NONE : state.rasterizer == RasterizerMode::SOLID_FRONT_CULL
        ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    // WHAT: GBufferなどのMRTはRT1以降にも法線・材質値を書き込む。
    // D3D12の初期値はWriteMask=0なので、RT0だけ設定するとDeferred Lighting入力が消える。
    for (uint32_t index = 0; index < renderTargetCount; ++index) {
        auto& blend = desc.BlendState.RenderTarget[index];
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (state.blend != BlendMode::OPAQUE_BLEND) {
            blend.BlendEnable = TRUE;
            // 方程式は RenderState.hpp の BlendMode が正本。ここはその翻訳でしかない。
            // PREMULTIPLIED は src.rgb に alpha が乗った値なので SrcBlend=ONE、
            // 背景側は (1-src.a) で残す (DX11 側の同名ケースと同じ方程式)。
            switch (state.blend) {
            case BlendMode::ADDITIVE:
                // SrcBlend は ONE ではない。ONE にすると出力アルファがブレンド方程式から
                // 消え、非事前乗算で書かれた PS (Particle.hlsl 等) が寿命フェードを失う。
                blend.SrcBlend  = D3D12_BLEND_SRC_ALPHA;
                blend.DestBlend = D3D12_BLEND_ONE;
                break;
            case BlendMode::PREMULTIPLIED:
                blend.SrcBlend  = D3D12_BLEND_ONE;
                blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                break;
            default: // ALPHA_BLEND
                blend.SrcBlend  = D3D12_BLEND_SRC_ALPHA;
                blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                break;
            }
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
    }

    desc.DepthStencilState.DepthEnable = state.depth != DepthMode::DEPTH_OFF;
    desc.DepthStencilState.DepthWriteMask = state.depth == DepthMode::DEPTH_ON
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = state.depth == DepthMode::DEPTH_SKY
        ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    if (FAILED(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) {
        FBZZ_LOG_ERROR("DX12PsoCache: PSO の生成に失敗しました: %s", shader.GetPath().c_str());
        return nullptr;
    }
    ID3D12PipelineState* result = pso.Get();
    m_cache.emplace(key, std::move(pso));
    FBZZ_LOG_DEBUG("DX12PsoCache: Graphics PSO生成 [%s] rtCount=%u format=%u topology=%u",
                  shader.GetPath().c_str(), renderTargetCount,
                  static_cast<unsigned>(renderTargetFormat), static_cast<unsigned>(topology));
    return result;
}

ID3D12PipelineState* DX12PsoCache::GetOrCreateCompute(const DX12Shader& shader)
{
    if (const auto found = m_computeCache.find(&shader); found != m_computeCache.end())
        return found->second.Get();
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_computeRootSignature.Get();
    desc.CS = shader.GetComputeBytecode();
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    if (FAILED(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)))) {
        FBZZ_LOG_ERROR("DX12PsoCache: Compute PSO生成失敗: %s", shader.GetPath().c_str());
        return nullptr;
    }
    ID3D12PipelineState* result = pso.Get();
    m_computeCache.emplace(&shader, std::move(pso));
    FBZZ_LOG_DEBUG("DX12PsoCache: Compute PSO生成 [%s]", shader.GetPath().c_str());
    return result;
}

} // namespace fbzz::renderer
