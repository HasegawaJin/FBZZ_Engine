// FBZZ Engine
// DX11Shader.cpp | fbzz::renderer
// DX11 シェーダーバイナリと InputLayout の管理
// ビルド済み CSO を読み込み、反射情報から入力レイアウトを生成する。
// 実行時コンパイルではなく、事前コンパイル済みシェーダーを前提にする。
#include "DX11Shader.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/HResult.hpp>
#include <Windows.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <string_view>

namespace fbzz::renderer
{

namespace {

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

} // namespace

// "assets/shaders/Debug/DebugDraw.hlsl"       -> "assets/shaders/compiled/Debug.DebugDraw"
// "assets/shaders/Material/Surface/PBR.hlsl"  -> "assets/shaders/compiled/Material.Surface.PBR"
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
    std::string rel  = normalized.substr(a + anchor.size());    // "Material/Surface/PBR.hlsl"

    if (rel == "Debug.hlsl") rel = "Debug/DebugDraw.hlsl";
    else if (rel == "DebugSelectionMask.hlsl") rel = "Debug/SelectionMask.hlsl";
    else if (rel == "DebugSelectionMaskSkinnedMesh.hlsl") rel = "Debug/SelectionMaskSkinnedMesh.hlsl";
    else if (rel == "Material/Unlit.hlsl") rel = "Material/Surface/Unlit.hlsl";
    else if (rel == "Material/Lit.hlsl") rel = "Material/Surface/Lit.hlsl";
    else if (rel == "Material/Phong.hlsl") rel = "Material/Surface/Phong.hlsl";
    else if (rel == "Material/BlinnPhong.hlsl") rel = "Material/Surface/BlinnPhong.hlsl";
    else if (rel == "Material/PBR.hlsl") rel = "Material/Surface/PBR.hlsl";
    else if (rel == "Material/Toon.hlsl") rel = "Material/Surface/Toon.hlsl";
    else if (rel == "Material/SkinnedPBR.hlsl") rel = "Material/Skinned/SkinnedPBR.hlsl";
    else if (rel == "Material/Particle.hlsl") rel = "Material/Effects/Particle.hlsl";
    else if (rel == "Pipeline/GBuffer.hlsl") rel = "Pipeline/Deferred/GBuffer.hlsl";
    else if (rel == "Pipeline/DeferredLighting.hlsl") rel = "Pipeline/Deferred/DeferredLighting.hlsl";
    else if (rel == "Pipeline/ShadowMap.hlsl") rel = "Pipeline/Shadow/ShadowMap.hlsl";
    else if (rel == "Pipeline/SkinnedShadowMap.hlsl") rel = "Pipeline/Shadow/SkinnedShadowMap.hlsl";
    else if (rel == "PostProcess/SSAO.cs.hlsl") rel = "PostProcess/AmbientOcclusion/SSAO.cs.hlsl";
    else if (rel == "PostProcess/SSAOBlur.cs.hlsl") rel = "PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl";
    else if (rel == "PostProcess/BloomDownsample.cs.hlsl") rel = "PostProcess/Bloom/BloomDownsample.cs.hlsl";
    else if (rel == "PostProcess/BloomUpsample.cs.hlsl") rel = "PostProcess/Bloom/BloomUpsample.cs.hlsl";
    else if (rel == "PostProcess/Composite.hlsl") rel = "PostProcess/Color/Composite.hlsl";
    else if (rel == "PostProcess/FXAA.hlsl") rel = "PostProcess/AntiAliasing/FXAA.hlsl";
    else if (rel == "PostProcess/SelectionOutline.hlsl") rel = "PostProcess/Outline/SelectionOutline.hlsl";

    // 元ファイルの拡張子を取り除く。
    size_t dot = rel.find_last_of('.');
    if (dot != std::string::npos) rel = rel.substr(0, dot); // "Material/Surface/PBR"

    // サブディレクトリをドット区切りに変換し、コンパイル済みシェーダー名に合わせる。
    for (char& c : rel)
        if (c == '/' || c == '\\') c = '.';

    return base + "compiled/" + rel; // "assets/shaders/compiled/Material.Surface.PBR"
}

// コンパイル済みシェーダーバイナリを byte 配列へ読み込む。
// D3D のシェーダー生成 API は生 byte ポインタとサイズを受け取る。
// byte 列を vector に保持することで、D3D 呼び出し中の保存領域を安定させる。
std::vector<uint8_t> DX11Shader::LoadBinary(const std::string& filePath)
{
    // WHY: カレントディレクトリまたは shader path に日本語が含まれる配布環境でも、
    //      CSO を Unicode パスで開けるよう UTF-8 から wide path に変換する。
    std::ifstream file(Utf8ToWide(filePath), std::ios::binary | std::ios::ate);
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

ShaderDescriptor DX11Shader::BuildDescriptor(const std::vector<uint8_t>& psBlob)
{
    ShaderDescriptor desc;

    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(psBlob.data(), psBlob.size(),
        __uuidof(ID3D11ShaderReflection),
        reinterpret_cast<void**>(refl.GetAddressOf()))))
        return desc;

    // ---- MaterialConstants (b2) から編集可能変数を列挙 ----
    auto* cb = refl->GetConstantBufferByName("MaterialConstants");
    if (cb)
    {
        D3D11_SHADER_BUFFER_DESC cbDesc{};
        if (SUCCEEDED(cb->GetDesc(&cbDesc)))
        {
            desc.cbufferSize = cbDesc.Size;
            for (UINT i = 0; i < cbDesc.Variables; ++i)
            {
                auto* var = cb->GetVariableByIndex(i);
                D3D11_SHADER_VARIABLE_DESC vDesc{};
                D3D11_SHADER_TYPE_DESC     tDesc{};
                if (FAILED(var->GetDesc(&vDesc))) continue;
                if (FAILED(var->GetType()->GetDesc(&tDesc))) continue;
                if (!vDesc.Name) continue;

                // WHY: D3D Reflection は最適化済みシェーダーや匿名パディング相当の変数で
                //      Name が null になる可能性がある。std::string_view(nullptr) は MSVC STL の
                //      strlen 経路でクラッシュするため、null は編集対象外として捨てる。
                std::string_view n = vDesc.Name;
                // パディング変数はスキップ
                if (n.starts_with("_")) continue;
                // textureMask は Inspector に出さないが offset を記録する
                if (n == "textureMask") {
                    desc.textureMaskOffset = vDesc.StartOffset;
                    continue;
                }

                ShaderVarDesc svd;
                svd.name    = vDesc.Name;
                svd.offset  = vDesc.StartOffset;
                svd.size    = vDesc.Size;
                svd.rows    = static_cast<uint8_t>(tDesc.Rows);
                svd.columns = static_cast<uint8_t>(tDesc.Columns);
                svd.varClass = (tDesc.Class == D3D_SVC_SCALAR) ? ShaderVarClass::Scalar
                             : (tDesc.Class == D3D_SVC_VECTOR) ? ShaderVarClass::Vector
                             :                                    ShaderVarClass::Matrix;
                svd.varType  = (tDesc.Type == D3D_SVT_FLOAT)   ? ShaderVarType::Float
                             : (tDesc.Type == D3D_SVT_INT)     ? ShaderVarType::Int
                             : (tDesc.Type == D3D_SVT_UINT)    ? ShaderVarType::UInt
                             :                                    ShaderVarType::Bool;
                desc.vars.push_back(std::move(svd));
            }
        }
    }

    // ---- t0-t4 のテクスチャバインドを列挙 ----
    D3D11_SHADER_DESC shDesc{};
    refl->GetDesc(&shDesc);
    for (UINT i = 0; i < shDesc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC bDesc{};
        refl->GetResourceBindingDesc(i, &bDesc);
        if (bDesc.Type == D3D_SIT_TEXTURE && bDesc.BindPoint < 5)
        {
            if (!bDesc.Name) continue;

            ShaderTexBindDesc t;
            t.name = bDesc.Name;
            t.slot = bDesc.BindPoint;
            desc.textures.push_back(std::move(t));
        }
    }
    std::sort(desc.textures.begin(), desc.textures.end(),
        [](const auto& a, const auto& b) { return a.slot < b.slot; });

    return desc;
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

    // PS バイトコードから MaterialConstants とテクスチャバインドを取得する。
    m_descriptor = BuildDescriptor(psBlob);

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
