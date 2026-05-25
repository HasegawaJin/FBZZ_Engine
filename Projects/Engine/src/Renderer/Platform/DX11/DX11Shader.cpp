// FBZZ Engine
// DX11Shader.cpp | fbzz::renderer
// DX11 シェーダーバイナリと InputLayout の管理
// ビルド済み CSO を読み込み、反射情報から入力レイアウトを生成する。
// 実行時コンパイルではなく、事前コンパイル済みシェーダーを前提にする。
#include "DX11Shader.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/HResult.hpp>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>

namespace fbzz::renderer
{

// "assets/shaders/Mesh.hlsl"         -> "assets/shaders/compiled/Mesh"
// "assets/shaders/Material/PBR.hlsl" -> "assets/shaders/compiled/Material.PBR"
// shaders/ アンカーより後ろの部分から CSO の基底パスを作る。
// コンパイル済み CSO は assets/shaders/compiled/ に置く。
std::string DX11Shader::CompiledBase(const std::string& path)
{
    std::string normalized = path;
    for (char& c : normalized)
        if (c == '\\') c = '/';

    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::string anchor = "shaders/";
    size_t a = lower.find(anchor);

    if (a == std::string::npos)
    {
        // shaders/ を含まない古いパス向けのフォールバック。
        size_t slash = normalized.find_last_of('/');
        size_t dot   = normalized.find_last_of('.');
        std::string dir  = (slash != std::string::npos) ? normalized.substr(0, slash + 1) : "";
        std::string stem = normalized.substr(slash + 1, dot - slash - 1);
        return dir + "compiled/" + stem;
    }

    std::string base = normalized.substr(0, a + anchor.size()); // "assets/shaders/"
    std::string rel  = normalized.substr(a + anchor.size());    // "Material/PBR.hlsl"

    // 元ファイルの拡張子を取り除く。
    size_t dot = rel.find_last_of('.');
    if (dot != std::string::npos) rel = rel.substr(0, dot); // "Material/PBR"

    // サブディレクトリをドット区切りに変換し、コンパイル済みシェーダー名に合わせる。
    for (char& c : rel)
        if (c == '/' || c == '\\') c = '.';

    return base + "compiled/" + rel; // "assets/shaders/compiled/Material.PBR"
}

// コンパイル済みシェーダーバイナリを byte 配列へ読み込む。
// D3D のシェーダー生成 API は生 byte ポインタとサイズを受け取る。
// byte 列を vector に保持することで、D3D 呼び出し中の保存領域を安定させる。
std::vector<uint8_t> DX11Shader::LoadBinary(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        FBZZ_LOG_ERROR("Shader binary open failed: %s", filePath.c_str());
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

    const std::string base = CompiledBase(path);

    // .cs.hlsl は VS/PS の組ではなく、単体のコンピュートシェーダー CSO として読み込む。
    const bool isCompute = (path.size() >= 8 &&
                             path.substr(path.size() - 8) == ".cs.hlsl");
    if (isCompute)
    {
        const std::string csPath = base + ".cso";
        auto csBlob = LoadBinary(csPath);
        if (csBlob.empty()) return false;

        FBZZ_HR_CHECK(device->CreateComputeShader(
            csBlob.data(), csBlob.size(), nullptr, m_computeShader.GetAddressOf()));
        return true;
    }

    const std::string vsPath = base + ".vs.cso";
    const std::string psPath = base + ".ps.cso";

    // -------------------------------------------------------------------------
    // Vertex Shader
    //   CSO byte 列を CreateVertexShader に渡す。
    //   input layout のリフレクション用に vsBlob を保持する。
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
    // VS の入力シグネチャをリフレクションし、D3D11_INPUT_ELEMENT_DESC を作る。
    // SV_VertexID のような system value semantic は input slot を必要としない。
    // -------------------------------------------------------------------------
    {
        // 頂点シェーダーのバイトコードをリフレクションする。
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> pReflector;
        HRESULT hrRefl = D3DReflect(vsBlob.data(), vsBlob.size(),
                                     __uuidof(ID3D11ShaderReflection),
                                     reinterpret_cast<void**>(pReflector.GetAddressOf()));
        if (FAILED(hrRefl))
        {
            FBZZ_LOG_ERROR("Shader reflection failed: %s", path.c_str());
            return false;
        }

        // 入力シグネチャを読む。
        D3D11_SHADER_DESC shaderDesc = {};
        pReflector->GetDesc(&shaderDesc);

        std::vector<D3D11_INPUT_ELEMENT_DESC> inputElements;
        std::vector<std::string>              semanticNames;
        semanticNames.reserve(shaderDesc.InputParameters);
        UINT byteOffset = 0;

        // リフレクションした入力パラメーターを input layout 要素へ変換する。
        for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
        {
            D3D11_SIGNATURE_PARAMETER_DESC paramDesc = {};
            pReflector->GetInputParameterDesc(i, &paramDesc);

            if (paramDesc.SystemValueType != D3D_NAME_UNDEFINED) continue;

            // CreateInputLayout が終わるまで semantic 文字列の寿命を保つ。
            semanticNames.emplace_back(paramDesc.SemanticName);

            D3D11_INPUT_ELEMENT_DESC elem   = {};
            elem.SemanticName               = semanticNames.back().c_str();
            elem.SemanticIndex              = paramDesc.SemanticIndex;
            elem.InputSlot                  = 0;  // 頂点バッファスロットは 1 つ
            elem.AlignedByteOffset          = byteOffset;
            elem.InputSlotClass             = D3D11_INPUT_PER_VERTEX_DATA;
            elem.InstanceDataStepRate       = 0;

            // component mask と型から対応する DXGI_FORMAT を選ぶ。
            const bool isUInt = paramDesc.ComponentType == D3D_REGISTER_COMPONENT_UINT32;
            if (paramDesc.Mask <= 0x1) {
                elem.Format = isUInt ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
                byteOffset += 4;
            }
            else if (paramDesc.Mask <= 0x3) {
                elem.Format = isUInt ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32_FLOAT;
                byteOffset += 8;
            }
            else if (paramDesc.Mask <= 0x7) {
                elem.Format = isUInt ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32_FLOAT;
                byteOffset += 12;
            }
            else {
                elem.Format = isUInt ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
                byteOffset += 16;
            }

            inputElements.push_back(elem);
        }

        // シェーダーが頂点属性を読む場合だけ InputLayout を作成する。
        // フルスクリーンパスは SV_VertexID だけを使うため input layout は null のままにする。
        if (!inputElements.empty()) {
            FBZZ_HR_CHECK(device->CreateInputLayout(
                inputElements.data(), static_cast<UINT>(inputElements.size()),
                vsBlob.data(), vsBlob.size(),
                m_inputLayout.GetAddressOf()));
        }
    }

    return true;
}

void DX11Shader::Bind(ID3D11DeviceContext* context) const
{
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(),  nullptr, 0);

    // 頂点バッファを送る前に、リフレクション済み input layout をバインドする。
    context->IASetInputLayout(m_inputLayout.Get());
}

} // namespace fbzz::renderer
