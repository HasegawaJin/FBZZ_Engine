// FBZZ Engine
// DX11Shader.cpp | fbzz::renderer
// DX11 頂点・ピクセルシェーダーと InputLayout の管理
#include "DX11Shader.hpp"
#include <engine/Core/Logger.hpp>
#include <engine/Core/HResult.hpp>
#include <fstream>
#include <vector>

namespace fbzz::renderer
{

// "assets/shaders/Phong.hlsl" → "assets/shaders/compiled/Phong"
// ファイル名のステム (拡張子を除いた部分) を compiled/ サブディレクトリへ移す。
// 実際のシェーダーファイルは CMake のビルドステップで事前コンパイルして配置する前提。
std::string DX11Shader::CompiledBase(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    size_t dot   = path.find_last_of('.');

    std::string dir  = (slash != std::string::npos) ? path.substr(0, slash + 1) : "";
    std::string stem = path.substr(slash + 1, dot - slash - 1);

    return dir + "compiled/" + stem;
}

// CSO バイナリを読み込んで std::vector<uint8_t> で返す。
// CreateVertexShader / CreatePixelShader は void* + size を要求するため、
// バイト列をそのまま渡せる vector が都合よい。
std::vector<uint8_t> DX11Shader::LoadBinary(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        FBZZ_LOG_ERROR("シェーダーファイルが開けません: %s", filePath.c_str());
        return {};
    }

    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0);

    std::vector<uint8_t> blob(size);
    file.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(size));
    return blob;
}

bool DX11Shader::Init(ID3D11Device* device, const std::string& path)
{
    m_path = path;

    const std::string base   = CompiledBase(path);
    const std::string vsPath = base + ".vs.cso";
    const std::string psPath = base + ".ps.cso";

    // -------------------------------------------------------------------------
    // Vertex Shader
    //   CSO バイナリを CreateVertexShader に渡す。
    //   vsBlob は InputLayout 生成にも再利用するため保持する。
    // -------------------------------------------------------------------------
    auto vsBlob = LoadBinary(vsPath);
    if (vsBlob.empty()) return false;

    FBZZ_HR_CHECK(device->CreateVertexShader(
        vsBlob.data(), vsBlob.size(), nullptr, m_vertexShader.GetAddressOf()));

    // -------------------------------------------------------------------------
    // Pixel Shader
    // -------------------------------------------------------------------------
    auto psBlob = LoadBinary(psPath);
    if (psBlob.empty()) return false;

    FBZZ_HR_CHECK(device->CreatePixelShader(
        psBlob.data(), psBlob.size(), nullptr, m_pixelShader.GetAddressOf()));

    // -------------------------------------------------------------------------
    // Input Layout
    //   頂点フォーマット: { POSITION(12B), NORMAL(12B), TEXCOORD(8B) } = 合計 32B
    //   セマンティクス名は HLSL 側の struct メンバ名と一致させる必要がある。
    //   InputLayout は VS バイトコードと照合して互換性を検証するため、
    //   vsBlob を引数として渡す (PS バイトコードは不要)。
    // -------------------------------------------------------------------------
    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    FBZZ_HR_CHECK(device->CreateInputLayout(
        layout, ARRAYSIZE(layout),
        vsBlob.data(), vsBlob.size(),
        m_inputLayout.GetAddressOf()));

    return true;
}

void DX11Shader::Bind(ID3D11DeviceContext* context) const
{
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(),  nullptr, 0);

    // InputLayout を IA (Input Assembler) ステージにバインドする。
    // IASetVertexBuffers より先に呼んでも後に呼んでも動作は同じだが、
    // 可読性のため Shader → InputLayout → VertexBuffer の順に揃えている。
    context->IASetInputLayout(m_inputLayout.Get());
}

} // namespace fbzz::renderer
