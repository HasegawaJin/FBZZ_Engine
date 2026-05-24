// FBZZ Engine
// DX11Shader.cpp | fbzz::renderer
// Manages DX11 shader binaries and reflected input layouts.
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
// Build the CSO base path from the part after the shaders/ anchor.
// Compiled CSOs live under assets/shaders/compiled/.
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
        // Fallback for legacy paths that do not contain shaders/.
        size_t slash = normalized.find_last_of('/');
        size_t dot   = normalized.find_last_of('.');
        std::string dir  = (slash != std::string::npos) ? normalized.substr(0, slash + 1) : "";
        std::string stem = normalized.substr(slash + 1, dot - slash - 1);
        return dir + "compiled/" + stem;
    }

    std::string base = normalized.substr(0, a + anchor.size()); // "assets/shaders/"
    std::string rel  = normalized.substr(a + anchor.size());    // "Material/PBR.hlsl"

    // Drop the source extension.
    size_t dot = rel.find_last_of('.');
    if (dot != std::string::npos) rel = rel.substr(0, dot); // "Material/PBR"

    // Convert subdirectories to the dotted compiled shader name.
    for (char& c : rel)
        if (c == '/' || c == '\\') c = '.';

    return base + "compiled/" + rel; // "assets/shaders/compiled/Material.PBR"
}

// Load a compiled shader binary into a byte vector.
// D3D shader creation consumes a raw byte pointer and byte size.
// Keeping the bytes in a vector gives stable storage for the call.
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

    // .cs.hlsl sources load a single compute shader CSO instead of VS/PS CSOs.
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
    //   Pass the CSO bytes to CreateVertexShader.
    //   Keep vsBlob for input-layout reflection.
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
    // Reflect the VS input signature to build D3D11_INPUT_ELEMENT_DESC entries.
    // System-value semantics such as SV_VertexID do not need input slots.
    // -------------------------------------------------------------------------
    {
        // Reflect the vertex shader bytecode.
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> pReflector;
        HRESULT hrRefl = D3DReflect(vsBlob.data(), vsBlob.size(),
                                     __uuidof(ID3D11ShaderReflection),
                                     reinterpret_cast<void**>(pReflector.GetAddressOf()));
        if (FAILED(hrRefl))
        {
            FBZZ_LOG_ERROR("Shader reflection failed: %s", path.c_str());
            return false;
        }

        // Read the input signature.
        D3D11_SHADER_DESC shaderDesc = {};
        pReflector->GetDesc(&shaderDesc);

        std::vector<D3D11_INPUT_ELEMENT_DESC> inputElements;
        std::vector<std::string>              semanticNames;
        semanticNames.reserve(shaderDesc.InputParameters);
        UINT byteOffset = 0;

        // Convert reflected input parameters into input-layout elements.
        for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
        {
            D3D11_SIGNATURE_PARAMETER_DESC paramDesc = {};
            pReflector->GetInputParameterDesc(i, &paramDesc);

            if (paramDesc.SystemValueType != D3D_NAME_UNDEFINED) continue;

            // Keep semantic strings alive until CreateInputLayout finishes.
            semanticNames.emplace_back(paramDesc.SemanticName);

            D3D11_INPUT_ELEMENT_DESC elem   = {};
            elem.SemanticName               = semanticNames.back().c_str();
            elem.SemanticIndex              = paramDesc.SemanticIndex;
            elem.InputSlot                  = 0;  // Single vertex-buffer slot.
            elem.AlignedByteOffset          = byteOffset;
            elem.InputSlotClass             = D3D11_INPUT_PER_VERTEX_DATA;
            elem.InstanceDataStepRate       = 0;

            // Convert component mask and type to the matching DXGI format.
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

        // Create an input layout when the shader consumes vertex attributes.
        // Fullscreen passes use SV_VertexID only and keep a null input layout.
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

    // Bind the reflected input layout before vertex buffers are submitted.
    context->IASetInputLayout(m_inputLayout.Get());
}

} // namespace fbzz::renderer
