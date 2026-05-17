// FBZZ Engine
// DX11Shader.hpp | fbzz::renderer
// DX11 頂点・ピクセルシェーダーと InputLayout の管理
//
// 設計方針:
//   実行時コンパイル (D3DCompile) ではなく、ビルド済み CSO (Compiled Shader Object) を
//   ロードする方式を採用。理由:
//     - d3dcompiler.dll への依存をなくし、配布バイナリを軽量化できる
//     - ランタイムコンパイルエラーを事前に検出できる
//   シェーダーパスは "assets/shaders/Phong.hlsl" 形式で受け取り、
//   "assets/shaders/compiled/Phong.vs.cso" / ".ps.cso" に解決する。
//
//   InputLayout は頂点フォーマット { POSITION(12B), NORMAL(12B), TEXCOORD(8B) } で固定。
//   複数フォーマットへの対応は Step 2 以降で検討する。
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <engine/Renderer/IShader.hpp>

namespace fbzz::renderer
{

class DX11Shader : public IShader
{
public:
    // path: "assets/shaders/Phong.hlsl" 形式
    // → assets/shaders/compiled/Phong.vs.cso / .ps.cso を読み込む
    bool Init(ID3D11Device* device, const std::string& path);

    // VS / PS / InputLayout を IA・VS・PS ステージへ一括バインドする
    void Bind(ID3D11DeviceContext* context) const;

    const std::string& GetPath() const override { return m_path; }

private:
    // "assets/shaders/Phong.hlsl" → "assets/shaders/compiled/Phong" に変換するヘルパー
    static std::string           CompiledBase(const std::string& path);

    // CSO ファイルをバイト列として読み込む (CreateVertexShader 等に渡すため)
    static std::vector<uint8_t>  LoadBinary(const std::string& filePath);

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_inputLayout;  // 頂点レイアウト定義
    std::string                                m_path;
};

} // namespace fbzz::renderer
