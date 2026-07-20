// FBZZ Engine
// DX11Shader.hpp | fbzz::renderer
// DX11 頂点・ピクセルシェーダーと InputLayout の管理
// IShader を継承し、CSO から作ったネイティブシェーダーを保持する。
// InputLayout は頂点シェーダーの反射情報から作る。
//
// 設計方針:
//   通常はビルド済み CSO (Compiled Shader Object) をロードする。
//   CSO が存在しない場合 (初回起動・compile_shaders.ps1 未実行) は
//   D3DCompileFromFile でオンデマンドコンパイルし、生成した CSO をキャッシュする。
//   WHY: DemoGame / StandaloneApp が compile_shaders.ps1 なしに動作するよう。
//        エディター向けの本番ワークフローは compile_shaders.ps1 が担う。
//   シェーダーパスは "assets/shaders/Phong.hlsl" 形式で受け取り、
//   "assets/shaders/compiled/Phong.vs.cso" / ".ps.cso" に解決する。
//
//   InputLayout は VS バイトコードを D3DReflect でリフレクションして自動構築する。
//   シェーダーごとに異なる頂点フォーマット (Unlit: POSITION+NORMAL+TEXCOORD,
//   Debug: POSITION+COLOR 等) に対応できる。
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>

namespace fbzz::renderer
{

class DX11Shader : public IShader
{
public:
    // path: "assets/shaders/Phong.hlsl" 形式
    // → assets/shaders/compiled/Phong.vs.cso / .ps.cso を読み込む
    bool Init(ID3D11Device* device, const std::string& path);

    // VS / PS / InputLayout を IA・VS・PS ステージへ一括バインドする (VS+PS シェーダー用)
    void Bind(ID3D11DeviceContext* context) const;

    // Compute Shader を取得する (.cs.hlsl として Init されたシェーダーのみ非 null)
    ID3D11ComputeShader* GetComputeShader() const { return m_computeShader.Get(); }

    const std::string&      GetPath()       const override { return m_path; }
    const ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }

private:
    // "assets/shaders/Phong.hlsl" → "assets/shaders/compiled/Phong" に変換するヘルパー
    static std::string           CompiledBase(const std::string& path);

    // CSO ファイルをバイト列として読み込む (CreateVertexShader 等に渡すため)
    static std::vector<uint8_t>  LoadBinary(const std::string& filePath);

    // PS バイトコードから MaterialConstants cbuffer とテクスチャバインドを解析する
    static ShaderDescriptor      BuildDescriptor(const std::vector<uint8_t>& psBlob);

    Microsoft::WRL::ComPtr<ID3D11VertexShader>  m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_computeShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>   m_inputLayout;
    std::string                                 m_path;
    ShaderDescriptor                            m_descriptor;
};

} // namespace fbzz::renderer
