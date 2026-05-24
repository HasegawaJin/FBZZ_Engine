// FBZZ Engine
// DX11PipelineState.hpp | fbzz::renderer
// DX11 パイプラインステート管理
// IPipelineState を継承し、RenderStateDesc を DX11 state 群へ変換する。
// 描画時は DX11Renderer がこの state をバインドする。
//
// 設計方針:
//   DX11 にはモノリシックな PSO (Pipeline State Object) が存在しないため、
//   ラスタライザ・ブレンド・深度ステンシルの 3 ステートをひとつのクラスにまとめる。
//   PipelineStateDesc をキーに Init() 時点でステートオブジェクトをキャッシュし、
//   Submit() ごとに Apply() を呼ぶだけで 3 ステートを一括バインドできる設計。
//
//   ※ DX12 では ID3D12PipelineState として統合されるため、Step 6 (DX12 移行) で
//      このクラスの対応実装に差し替えるだけでよい。
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <Engine/Renderer/IPipelineState.hpp>

namespace fbzz::renderer
{

class DX11PipelineState : public IPipelineState
{
public:
    // desc の内容に従い、3 つの DX11 ステートオブジェクトを生成してキャッシュする
    bool Init(ID3D11Device* device, const PipelineStateDesc& desc);

    // RS / OM ステージへ 3 ステートを一括バインドする
    void Apply(ID3D11DeviceContext* context) const;

    const PipelineStateDesc& GetDesc() const override { return m_desc; }

private:
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blendState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    PipelineStateDesc                               m_desc;
};

} // namespace fbzz::renderer
