/// @file    DX12PsoCache.cpp
/// @brief   固定スロット Root Signature と描画状態別 PSO の遅延構築。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12PsoCache.hpp"

#include "DX12Context.hpp"
#include "DX12Shader.hpp"
#include <Core/Logger.hpp>
#include <Graphics/Renderer/BindlessIndices.hpp>
#include <array>
#include <d3dcompiler.h>

namespace fbzz::renderer {

namespace {

/// @note 静的サンプラーはシェーダーレジスタ (s0〜) 単位で決まる。Assets/Shaders/Common/Binding.hlsli の
/// @note SAMPLER_* 定義と 1:1 で対応させること。
/// @note DX12 は Root Signature へ焼き込む静的サンプラーのため、レジスタごとに 1 つの意味に固定
/// @note するしかない。両バックエンドをこの制約に合わせ、パス単位でサンプラーを差し替える API
/// @note (旧 IRenderer::SetSampler) は廃止した。以前は SamplerMode の列挙順をそのままレジスタ
/// @note 番号にしていたため、s1 が比較サンプラーでなく通常 Linear に、s4 が wrap でなく clamp に
/// @note ずれ、前者は影が全面に出て後者は 3D ノイズが端に張り付いていた。配列の並びを崩さないこと。
std::array<D3D12_STATIC_SAMPLER_DESC, 9> MakeStaticSamplers()
{
    struct Preset {
        D3D12_FILTER filter;
        D3D12_TEXTURE_ADDRESS_MODE address;
        UINT maxAnisotropy;
        D3D12_COMPARISON_FUNC comparison;
    };
    /// @note s6 / s8 は現状どのシェーダーも宣言していない予約枠。
    constexpr Preset kPresets[9] = {
        /// @note s0 SAMPLER_DEFAULT      : メッシュテクスチャのタイリングが主用途
        { D3D12_FILTER_ANISOTROPIC,                       D3D12_TEXTURE_ADDRESS_MODE_WRAP,   16, D3D12_COMPARISON_FUNC_NEVER },
        /// @note s1 SAMPLER_SHADOW       : SamplerComparisonState (PCF)
        { D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 1, D3D12_COMPARISON_FUNC_LESS_EQUAL },
        /// @note s2 SAMPLER_LINEAR_CLAMP : IBL BRDF LUT / 3D LUT / スプラットマップ
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR,                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   1, D3D12_COMPARISON_FUNC_NEVER },
        /// @note s3 SAMPLER_POINT_CLAMP  : TAA 再投影ルックアップ
        { D3D12_FILTER_MIN_MAG_MIP_POINT,                 D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   1, D3D12_COMPARISON_FUNC_NEVER },
        /// @note s4 SAMPLER_WRAP_LINEAR  : ボリューメトリック雲のタイラブル 3D ノイズ
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR,                D3D12_TEXTURE_ADDRESS_MODE_WRAP,    1, D3D12_COMPARISON_FUNC_NEVER },
        /// @note s5                      : UI スプライト / テキスト
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR,                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   1, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_POINT,                 D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   1, D3D12_COMPARISON_FUNC_NEVER },
        /// @note s7 SAMPLER_SHADOW_PUNCTUAL : Spot / Point 用の 2 本目の比較サンプラー。
        /// @note 設定は s1 と同一。別スロットにするのは、共有ヘッダー (PunctualShadow.hlsli) が
        /// @note 自前の名前で宣言する必要があり、s1 は各マテリアルシェーダーが既に占有しているため。
        { D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 1, D3D12_COMPARISON_FUNC_LESS_EQUAL },
        { D3D12_FILTER_ANISOTROPIC,                       D3D12_TEXTURE_ADDRESS_MODE_WRAP,    4, D3D12_COMPARISON_FUNC_NEVER },
    };

    std::array<D3D12_STATIC_SAMPLER_DESC, 9> samplers{};
    for (UINT slot = 0; slot < samplers.size(); ++slot) {
        auto& sampler = samplers[slot];
        sampler.Filter = kPresets[slot].filter;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = kPresets[slot].address;
        sampler.MaxAnisotropy = kPresets[slot].maxAnisotropy;
        sampler.ComparisonFunc = kPresets[slot].comparison;
        /// @note ライト錐台外のシャドウサンプルは「照らされている」に倒す。
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = slot;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    return samplers;
}

}

size_t DX12PsoCache::KeyHash::operator()(const Key& key) const
{
    size_t hash = std::hash<uint64_t>{}(key.shaderIdentity);
    hash ^= static_cast<size_t>(key.rasterizer) << 3;
    hash ^= static_cast<size_t>(key.blend) << 7;
    hash ^= static_cast<size_t>(key.depth) << 11;
    hash ^= static_cast<size_t>(key.topology) << 15;
    hash ^= static_cast<size_t>(key.renderTargetFormat) << 19;
    hash ^= static_cast<size_t>(key.renderTargetCount) << 25;
    hash ^= static_cast<size_t>(key.reversedZ) << 30;
    hash ^= std::hash<int32_t>{}(key.depthBias) * 31u;
    hash ^= std::hash<float>{}(key.depthBiasSlope) * 131u;
    return hash;
}

bool DX12PsoCache::Initialize(ID3D12Device* device, bool bindlessEnabled)
{
    if (!device) return false;
    m_device = device;
    m_bindlessEnabled = bindlessEnabled;
    return CreateRootSignature() && CreateComputeRootSignature();
}

D3D12_ROOT_SIGNATURE_FLAGS DX12PsoCache::BaseRootSignatureFlags() const
{
    /// @note 既存のディスクリプタテーブルを残したまま立てる理由: このフラグは «ヒープを直接引ける»
    /// @note という許可を足すだけで、テーブル経由の束縛を無効化しない。144 本のシェーダーを
    /// @note 一斉に書き換えずに、bindless へ移したものから順に切り替えられる。
    return m_bindlessEnabled
        ? D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        : D3D12_ROOT_SIGNATURE_FLAG_NONE;
}

void DX12PsoCache::Shutdown()
{
    ClearPipelines();
    m_computeRootSignature.Reset();
    m_rootSignature.Reset();
    m_device.Reset();
}

void DX12PsoCache::ClearPipelines()
{
    m_cache.clear();
    m_computeCache.clear();
}

bool DX12PsoCache::CreateComputeRootSignature()
{
    /// @note 並びは描画側と揃える (param 14 = bindless 添字ブロック b14)。
    /// @note SRV / UAV テーブルは撤去済み。CS も ResourceDescriptorHeap から直接引く。
    std::array<D3D12_ROOT_PARAMETER1, 15> parameters{};
    for (UINT slot = 0; slot < 14; ++slot) {
        parameters[slot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[slot].Descriptor.ShaderRegister = slot;
        parameters[slot].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameters[slot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[kBindlessIndicesRootParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[kBindlessIndicesRootParam].Descriptor.ShaderRegister = kBindlessIndicesRegister;
    parameters[kBindlessIndicesRootParam].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    parameters[kBindlessIndicesRootParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const auto samplers = MakeStaticSamplers();
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    desc.Desc_1_1.pParameters = parameters.data();
    desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    desc.Desc_1_1.pStaticSamplers = samplers.data();
    desc.Desc_1_1.Flags = BaseRootSignatureFlags();
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
    /// @note param 0〜13 : 定数バッファ b0〜b13 (root CBV)
    /// @note param 14    : bindless 添字ブロック b14 (root CBV)
    /// @note     /// @note かつてここに «ピクセル SRV テーブル (t0〜t31)» と «頂点 SRV テーブル» があったが、
    /// @note 全シェーダーが ResourceDescriptorHeap から直接引くようになったため撤去した。
    /// @note ディスクリプタテーブルが 1 つも無いので、ドローごとの CopyDescriptors も消えている。
    /// @note       @see Docs/design/bindless.md
    std::array<D3D12_ROOT_PARAMETER1, 15> parameters{};
    for (UINT slot = 0; slot < 14; ++slot) {
        parameters[slot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[slot].Descriptor.ShaderRegister = slot;
        parameters[slot].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameters[slot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    /// @note bindless 添字ブロック (b14)。VS / PS / GS のどこからでも引くので ALL 可視。
    parameters[kBindlessIndicesRootParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[kBindlessIndicesRootParam].Descriptor.ShaderRegister = kBindlessIndicesRegister;
    parameters[kBindlessIndicesRootParam].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    parameters[kBindlessIndicesRootParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    const auto samplers = MakeStaticSamplers();

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    desc.Desc_1_1.pParameters = parameters.data();
    desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    desc.Desc_1_1.pStaticSamplers = samplers.data();
    desc.Desc_1_1.Flags = BaseRootSignatureFlags()
        | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
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
    DXGI_FORMAT renderTargetFormat, uint32_t renderTargetCount, bool reversedZ)
{
    const Key key{shader.GetCacheIdentity(), state.rasterizer, state.blend, state.depth, topology,
                  renderTargetFormat, renderTargetCount, reversedZ,
                  state.depthBias, state.depthBiasSlope};
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

    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc FillMode と CullMode は独立した状態。
    desc.RasterizerState.FillMode = (state.rasterizer == RasterizerMode::WIREFRAME
        || state.rasterizer == RasterizerMode::WIREFRAME_NOCULL)
        ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = (state.rasterizer == RasterizerMode::SOLID_NOCULL
        || state.rasterizer == RasterizerMode::WIREFRAME_NOCULL)
        ? D3D12_CULL_MODE_NONE : state.rasterizer == RasterizerMode::SOLID_FRONT_CULL
        ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    /// @note PipelineStateDesc のバイアスは «正で手前»。手前ほど深度が大きい Reversed-Z ではそのまま、通常の Z では符号を裏返す。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc (D3D12_RASTERIZER_DESC)
    desc.RasterizerState.DepthBias = reversedZ ? state.depthBias : -state.depthBias;
    desc.RasterizerState.SlopeScaledDepthBias = reversedZ ? state.depthBiasSlope : -state.depthBiasSlope;

    /// @note GBuffer などの MRT は RT1 以降にも法線・材質値を書き込む。D3D12 の初期値は
    /// @note WriteMask=0 なので、RT0 だけ設定すると Deferred Lighting 入力が消える。
    for (uint32_t index = 0; index < renderTargetCount; ++index) {
        auto& blend = desc.BlendState.RenderTarget[index];
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (state.blend != BlendMode::OPAQUE_BLEND) {
            blend.BlendEnable = TRUE;
            /// @note 方程式は RenderState.hpp の BlendMode が正本。ここはその翻訳でしかない。
            /// @note PREMULTIPLIED は src.rgb に alpha が乗った値なので SrcBlend=ONE、
            /// @note 背景側は (1-src.a) で残す (DX11 側の同名ケースと同じ方程式)。
            switch (state.blend) {
            case BlendMode::ADDITIVE:
                /// @note SrcBlend は ONE ではない。ONE にすると出力アルファがブレンド方程式から
                /// @note 消え、非事前乗算で書かれた PS (Particle.hlsl 等) が寿命フェードを失う。
                blend.SrcBlend  = D3D12_BLEND_SRC_ALPHA;
                blend.DestBlend = D3D12_BLEND_ONE;
                break;
            case BlendMode::PREMULTIPLIED:
                blend.SrcBlend  = D3D12_BLEND_ONE;
                blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                break;
            /// @note ALPHA_BLEND
            default:
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
    /// @note Reversed-Z の RT では手前ほど深度が大きいので比較を裏返す。DepthMode は «手前が勝つ» の意味で書く。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_comparison_func (D3D12_COMPARISON_FUNC)
    if (reversedZ) {
        desc.DepthStencilState.DepthFunc = state.depth == DepthMode::DEPTH_SKY
            ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_GREATER;
    } else {
        desc.DepthStencilState.DepthFunc = state.depth == DepthMode::DEPTH_SKY
            ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_LESS;
    }
    desc.DepthStencilState.StencilEnable = FALSE;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    const HRESULT createResult = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(createResult)) {
        FBZZ_LOG_ERROR("DX12PsoCache: PSO の生成に失敗しました: %s (HRESULT=0x%08X, removed=0x%08X)",
                       shader.GetPath().c_str(), static_cast<unsigned>(createResult),
                       static_cast<unsigned>(m_device->GetDeviceRemovedReason()));
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
    const uint64_t identity = shader.GetCacheIdentity();
    if (const auto found = m_computeCache.find(identity); found != m_computeCache.end())
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
    m_computeCache.emplace(identity, std::move(pso));
    FBZZ_LOG_DEBUG("DX12PsoCache: Compute PSO生成 [%s]", shader.GetPath().c_str());
    return result;
}

}
