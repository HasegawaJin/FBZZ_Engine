// FBZZ Engine
// DX11PipelineState.cpp | fbzz::renderer
// DX11 パイプラインステート (ラスタライザ・ブレンド・深度ステンシル) 管理
#include "DX11PipelineState.hpp"
#include <engine/Core/HResult.hpp>

namespace fbzz::renderer
{

bool DX11PipelineState::Init(ID3D11Device* device, const PipelineStateDesc& desc)
{
    m_desc = desc;

    // -------------------------------------------------------------------------
    // Rasterizer State
    //   ポリゴンの塗り方 (SOLID / WIREFRAME) と背面カリング方向を設定する。
    //   FrontCounterClockwise = FALSE は DX の標準 (時計回りが表面)。
    // -------------------------------------------------------------------------
    {
        D3D11_RASTERIZER_DESC rsDesc = {};
        rsDesc.FillMode              = (desc.rasterizer == RasterizerMode::WIREFRAME)
                                       ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        rsDesc.CullMode              = D3D11_CULL_BACK;
        rsDesc.FrontCounterClockwise = FALSE;
        rsDesc.DepthClipEnable       = TRUE;  // ビューフラスタム外のジオメトリをクリップ

        FBZZ_HR_CHECK(device->CreateRasterizerState(&rsDesc, m_rasterizerState.GetAddressOf()));
    }

    // -------------------------------------------------------------------------
    // Blend State
    //   RenderTarget[0] のみ設定。ブレンド方程式:
    //     ALPHA_BLEND : out = src.rgb * src.a + dst.rgb * (1 - src.a)  (標準アルファ合成)
    //     ADDITIVE    : out = src.rgb * src.a + dst.rgb                 (加算。エフェクト向け)
    //     OPAQUE      : ブレンドなし (最も高速)
    // -------------------------------------------------------------------------
    {
        D3D11_BLEND_DESC bsDesc = {};
        auto& rt                = bsDesc.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        switch (desc.blend)
        {
        case BlendMode::ALPHA_BLEND:
            rt.BlendEnable    = TRUE;
            rt.SrcBlend       = D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp        = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha  = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOpAlpha   = D3D11_BLEND_OP_ADD;
            break;

        case BlendMode::ADDITIVE:
            rt.BlendEnable    = TRUE;
            rt.SrcBlend       = D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend      = D3D11_BLEND_ONE;   // dst を 1 倍で加算 → 発光感
            rt.BlendOp        = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha  = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOpAlpha   = D3D11_BLEND_OP_ADD;
            break;

        default: // OPAQUE
            rt.BlendEnable = FALSE;
            break;
        }

        FBZZ_HR_CHECK(device->CreateBlendState(&bsDesc, m_blendState.GetAddressOf()));
    }

    // -------------------------------------------------------------------------
    // Depth Stencil State
    //   DEPTH_ON   : 深度テスト + 書き込みあり (不透明オブジェクト標準)
    //   DEPTH_READ : 深度テストあり / 書き込みなし (半透明オブジェクト: 背面を隠すが自分は書かない)
    //   DEPTH_OFF  : 深度テスト・書き込みなし (デバッグ描画・UI: 常に最前面)
    // -------------------------------------------------------------------------
    {
        D3D11_DEPTH_STENCIL_DESC dsDesc = {};
        dsDesc.StencilEnable            = FALSE;  // ステンシルは現時点で未使用

        switch (desc.depth)
        {
        case DepthMode::DEPTH_READ:
            dsDesc.DepthEnable    = TRUE;
            dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;  // 書き込みなし
            dsDesc.DepthFunc      = D3D11_COMPARISON_LESS;
            break;

        case DepthMode::DEPTH_OFF:
            dsDesc.DepthEnable    = FALSE;
            dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dsDesc.DepthFunc      = D3D11_COMPARISON_ALWAYS;
            break;

        default: // DEPTH_ON
            dsDesc.DepthEnable    = TRUE;
            dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            dsDesc.DepthFunc      = D3D11_COMPARISON_LESS;
            break;
        }

        FBZZ_HR_CHECK(device->CreateDepthStencilState(&dsDesc, m_depthStencilState.GetAddressOf()));
    }

    return true;
}

void DX11PipelineState::Apply(ID3D11DeviceContext* context) const
{
    // RS ステージ: ラスタライザ設定を適用
    context->RSSetState(m_rasterizerState.Get());

    // OM ステージ: ブレンド + 深度ステンシルを適用
    // 第 2 引数 (BlendFactor) は nullptr → シェーダー側の SRC_BLEND_FACTOR で代替される
    // 第 3 引数 (SampleMask) は 0xFFFFFFFF → 全サンプルに適用
    context->OMSetBlendState(m_blendState.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
}

} // namespace fbzz::renderer
