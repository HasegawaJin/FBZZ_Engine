// FBZZ Engine
// DX12Shader.cpp | fbzz::renderer
// DXC / SM 6.8 による DXIL コンパイルと DXBC / DXIL 両対応リフレクション
#include "DX12Shader.hpp"

#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ShaderCompileDiagnostics.hpp>
#include <Engine/Renderer/ShaderDependencyTracker.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cwctype>
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <Windows.h>
#include <wrl/client.h>

namespace fbzz::renderer {

namespace {

// 相対Assetsパスを実行ディレクトリから上方向へ探索する。
// WHY: Sandbox.exeの作業ディレクトリとリポジトリ直下のどちらから起動してもHLSLを発見するため。
std::wstring ResolveShaderPath(const std::string& path)
{
    const std::wstring requested = util::StringUtils::ToWide(path);
    if (GetFileAttributesW(requested.c_str()) != INVALID_FILE_ATTRIBUTES) return requested;
    wchar_t cwd[MAX_PATH]{};
    if (GetCurrentDirectoryW(MAX_PATH, cwd) == 0) return requested;
    std::wstring current = cwd;
    for (;;) {
        const std::wstring candidate = current + L"/" + requested;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
        const size_t slash = current.find_last_of(L"/\\");
        if (slash == std::wstring::npos) break;
        current.resize(slash);
    }
    return requested;
}

// dxcompiler.dll を遅延ロードして DXC API の静的リンク依存を避ける。
// WHY: Windows SDK や同梱 DXC の配置差を吸収し、DX11 のみを使う環境では DLL を要求しない。
DxcCreateInstanceProc GetDxcCreateInstance()
{
    static HMODULE module = [] {
        HMODULE loaded = LoadLibraryW(L"dxcompiler.dll");
        if (loaded) return loaded;

        // FBZZ_DXC が dxc.exe の絶対パスなら、同じディレクトリの DLL も探索する。
        wchar_t compilerPath[32768]{};
        const DWORD length = GetEnvironmentVariableW(
            L"FBZZ_DXC", compilerPath, static_cast<DWORD>(std::size(compilerPath)));
        if (length == 0 || length >= std::size(compilerPath)) return static_cast<HMODULE>(nullptr);
        std::filesystem::path dllPath(compilerPath);
        dllPath.replace_filename(L"dxcompiler.dll");
        return LoadLibraryW(dllPath.c_str());
    }();
    if (!module) return nullptr;
    return std::bit_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
}

bool CreateDxcServices(Microsoft::WRL::ComPtr<IDxcUtils>& utils,
                       Microsoft::WRL::ComPtr<IDxcCompiler3>* compiler = nullptr)
{
    const DxcCreateInstanceProc createInstance = GetDxcCreateInstance();
    if (!createInstance) return false;
    if (FAILED(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) return false;
    return !compiler || SUCCEEDED(createInstance(
        CLSID_DxcCompiler, IID_PPV_ARGS(compiler->ReleaseAndGetAddressOf())));
}

// DXBC は D3DReflect、DXIL は DXC の container reflection で同じ D3D12 API に正規化する。
Microsoft::WRL::ComPtr<ID3D12ShaderReflection> CreateShaderReflection(
    const std::vector<uint8_t>& blob)
{
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
    if (SUCCEEDED(D3DReflect(blob.data(), blob.size(), IID_PPV_ARGS(&reflection))))
        return reflection;

    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    if (!CreateDxcServices(utils)) return {};
    DxcBuffer buffer{};
    buffer.Ptr = blob.data();
    buffer.Size = blob.size();
    buffer.Encoding = DXC_CP_ACP;
    if (FAILED(utils->CreateReflection(&buffer, IID_PPV_ARGS(&reflection)))) return {};
    return reflection;
}

// HLSLをDX12定義付きで SM 6.8 DXIL へコンパイルし、次回起動用に保存する。
std::vector<uint8_t> CompileShader(
    const std::string& path, const char* entryPoint, const char* target,
    const std::string& csoSavePath)
{
    ClearShaderCompileDiagnosticsFor(path, entryPoint, target);
    const std::wstring resolved = ResolveShaderPath(path);
    std::filesystem::path shaderRoot = std::filesystem::path(resolved).parent_path();
    while (!shaderRoot.empty()) {
        std::wstring name = shaderRoot.filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        if (name == L"shaders") break;
        shaderRoot = shaderRoot.parent_path();
    }
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    if (!CreateDxcServices(utils, &compiler)) {
        const std::string message =
            "dxcompiler.dll を読み込めません。DXC を実行ファイルの隣へ配置するか FBZZ_DXC を設定してください";
        FBZZ_LOG_ERROR("DX12Shader: %s", message.c_str());
        ReportShaderCompileDiagnostic(path, entryPoint, target, message, true);
        return {};
    }

    Microsoft::WRL::ComPtr<IDxcBlobEncoding> source;
    if (FAILED(utils->LoadFile(resolved.c_str(), nullptr, &source))) {
        const std::string message = "HLSLを読み込めません";
        FBZZ_LOG_ERROR("DX12Shader: %s: %s", message.c_str(), path.c_str());
        ReportShaderCompileDiagnostic(path, entryPoint, target, message, true);
        return {};
    }
    const std::wstring wideEntry = util::StringUtils::ToWide(entryPoint);
    const std::wstring wideTarget = util::StringUtils::ToWide(target);
    const std::wstring includePath = shaderRoot.wstring();
    std::vector<LPCWSTR> arguments = {
        resolved.c_str(), L"-E", wideEntry.c_str(), L"-T", wideTarget.c_str(),
        L"-D", L"FBZZ_BACKEND_DX12=1", L"-I", includePath.c_str(), L"-HV", L"2021"
    };
#if defined(_DEBUG)
    arguments.insert(arguments.end(), {L"-Zi", L"-Qembed_debug", L"-Od"});
#else
    arguments.push_back(L"-O3");
#endif
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> includeHandler;
    if (FAILED(utils->CreateDefaultIncludeHandler(&includeHandler))) return {};
    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = source->GetBufferPointer();
    sourceBuffer.Size = source->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;
    Microsoft::WRL::ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&sourceBuffer, arguments.data(),
                                 static_cast<uint32_t>(arguments.size()), includeHandler.Get(),
                                 IID_PPV_ARGS(&result)))) {
        const std::string message = "DXCコンパイラの呼び出しに失敗しました";
        FBZZ_LOG_ERROR("DX12Shader: %s: %s", message.c_str(), path.c_str());
        ReportShaderCompileDiagnostic(path, entryPoint, target, message, true);
        return {};
    }

    Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (errors && errors->GetStringLength() > 0) {
        const std::string message(errors->GetStringPointer(), errors->GetStringLength());
        if (FAILED(status))
            FBZZ_LOG_ERROR("DX12Shader: DXCコンパイル失敗 %s (%s/%s)\n%s",
                           path.c_str(), entryPoint, target, message.c_str());
        else
            FBZZ_LOG_WARN("DX12Shader: DXCコンパイル警告 %s\n%s",
                          path.c_str(), message.c_str());
        ReportShaderCompileDiagnostic(path, entryPoint, target, message, FAILED(status));
    }
    Microsoft::WRL::ComPtr<IDxcBlob> code;
    if (FAILED(status) || FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr)) || !code)
        return {};

    // 再コンパイル結果を保存し、次回起動時のコンパイルを避ける。
    if (!csoSavePath.empty()) {
        std::filesystem::path output = shader_dependency::ResolveExistingPath(
            util::StringUtils::ToWide(csoSavePath));
        std::error_code error;
        if (!std::filesystem::is_regular_file(output, error)) {
            const std::filesystem::path sourcePath(resolved);
            const std::filesystem::path resolvedShaderRoot =
                shader_dependency::FindShaderRoot(sourcePath);
            std::string normalized = csoSavePath;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            std::string lower = normalized;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string anchor = "shaders/";
            const size_t anchorIndex = lower.find(anchor);
            output = anchorIndex == std::string::npos
                ? resolvedShaderRoot / std::filesystem::path(normalized).filename()
                : resolvedShaderRoot / util::StringUtils::ToWide(
                    normalized.substr(anchorIndex + anchor.size()));
        }
        error.clear();
        std::filesystem::create_directories(output.parent_path(), error);
        std::ofstream binary(output, std::ios::binary);
        if (binary.is_open()) {
            binary.write(static_cast<const char*>(code->GetBufferPointer()),
                         static_cast<std::streamsize>(code->GetBufferSize()));
            FBZZ_LOG_INFO("DX12Shader: CSOを更新しました: %s", csoSavePath.c_str());
        } else {
            FBZZ_LOG_WARN("DX12Shader: CSOを保存できません: %s", csoSavePath.c_str());
        }
    }
    const auto* begin = static_cast<const uint8_t*>(code->GetBufferPointer());
    return {begin, begin + code->GetBufferSize()};
}

} // namespace

std::string DX12Shader::CompiledBase(const std::string& path)
{
    std::string normalized = path;
    for (char& c : normalized) if (c == '\\') c = '/';
    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string anchor = "shaders/";
    const size_t anchorIndex = lower.find(anchor);
    if (anchorIndex == std::string::npos) {
        const size_t slash = normalized.find_last_of('/');
        const size_t dot = normalized.find_last_of('.');
        return normalized.substr(0, slash + 1) + "compiled_dx12/"
             + normalized.substr(slash + 1, dot - slash - 1);
    }
    std::string base = normalized.substr(0, anchorIndex + anchor.size());
    std::string relative = normalized.substr(anchorIndex + anchor.size());

    // WHAT: 既存アセットの互換エイリアスを DX11 と同じ CSO 名へ正規化する。
    if (relative == "Debug.hlsl") relative = "Debug/DebugDraw.hlsl";
    else if (relative == "Material/Unlit.hlsl") relative = "Material/Surface/Unlit.hlsl";
    else if (relative == "Material/Lit.hlsl") relative = "Material/Surface/Lit.hlsl";
    else if (relative == "Material/Phong.hlsl") relative = "Material/Surface/Phong.hlsl";
    else if (relative == "Material/BlinnPhong.hlsl") relative = "Material/Surface/BlinnPhong.hlsl";
    else if (relative == "Material/PBR.hlsl") relative = "Material/Surface/PBR.hlsl";
    else if (relative == "Material/Toon.hlsl") relative = "Material/Surface/Toon.hlsl";
    else if (relative == "Material/SkinnedPBR.hlsl") relative = "Material/Skinned/SkinnedPBR.hlsl";
    else if (relative == "Material/Particle.hlsl") relative = "Material/Effects/Particle.hlsl";
    else if (relative == "Pipeline/GBuffer.hlsl") relative = "Pipeline/Deferred/GBuffer.hlsl";
    else if (relative == "Pipeline/DeferredLighting.hlsl") relative = "Pipeline/Deferred/DeferredLighting.hlsl";
    else if (relative == "Pipeline/ShadowMap.hlsl") relative = "Pipeline/Shadow/ShadowMap.hlsl";

    const size_t dot = relative.find_last_of('.');
    if (dot != std::string::npos) relative.resize(dot);
    for (char& c : relative) if (c == '/' || c == '\\') c = '.';
    return base + "compiled_dx12/" + relative;
}

ShaderDescriptor DX12Shader::BuildDescriptor(const std::vector<uint8_t>& psBlob)
{
    // WHAT: DXBC / DXIL の PS reflection を共通化し、DX11 と同じ descriptor を構築する。
    // WHY: Inspector / SyncMaterial / サムネイルへバックエンド差を漏らさないため。
    ShaderDescriptor desc;

    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> refl = CreateShaderReflection(psBlob);
    if (!refl) return desc;

    // ---- MaterialConstants (b2) から編集可能変数を列挙 ----
    auto* cb = refl->GetConstantBufferByName("MaterialConstants");
    if (cb)
    {
        D3D12_SHADER_BUFFER_DESC cbDesc{};
        if (SUCCEEDED(cb->GetDesc(&cbDesc)))
        {
            desc.cbufferSize = cbDesc.Size;
            for (UINT i = 0; i < cbDesc.Variables; ++i)
            {
                auto* var = cb->GetVariableByIndex(i);
                D3D12_SHADER_VARIABLE_DESC vDesc{};
                D3D12_SHADER_TYPE_DESC     tDesc{};
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

    // ---- PostProcConstants (b5) から custom* 変数を列挙 ----
    // WHY: 共通 PostProcess Inspector が実際にシェーダーで使用される入力だけを表示するため。
    if (auto* postCb = refl->GetConstantBufferByName("PostProcConstants"))
    {
            D3D12_SHADER_BUFFER_DESC cbDesc{};
        if (SUCCEEDED(postCb->GetDesc(&cbDesc)))
        {
            for (UINT i = 0; i < cbDesc.Variables; ++i)
            {
                auto* var = postCb->GetVariableByIndex(i);
                D3D12_SHADER_VARIABLE_DESC vDesc{};
                D3D12_SHADER_TYPE_DESC tDesc{};
                if (FAILED(var->GetDesc(&vDesc)) || FAILED(var->GetType()->GetDesc(&tDesc)) || !vDesc.Name)
                    continue;
                const std::string_view name = vDesc.Name;
                if (!name.starts_with("custom")) continue;

                ShaderVarDesc value;
                value.name = vDesc.Name;
                value.offset = vDesc.StartOffset;
                value.size = vDesc.Size;
                value.rows = static_cast<uint8_t>(tDesc.Rows);
                value.columns = static_cast<uint8_t>(tDesc.Columns);
                value.varClass = (tDesc.Class == D3D_SVC_SCALAR) ? ShaderVarClass::Scalar
                               : (tDesc.Class == D3D_SVC_VECTOR) ? ShaderVarClass::Vector
                               :                                    ShaderVarClass::Matrix;
                value.varType = (tDesc.Type == D3D_SVT_FLOAT) ? ShaderVarType::Float
                              : (tDesc.Type == D3D_SVT_INT)   ? ShaderVarType::Int
                              : (tDesc.Type == D3D_SVT_UINT)  ? ShaderVarType::UInt
                              :                                  ShaderVarType::Bool;
                desc.postProcessVars.push_back(std::move(value));
            }
        }
    }

    // ---- t0-t4 のテクスチャバインドを列挙 ----
    D3D12_SHADER_DESC shDesc{};
    refl->GetDesc(&shDesc);
    for (UINT i = 0; i < shDesc.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC bDesc{};
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

std::vector<uint8_t> DX12Shader::LoadBinary(const std::string& path)
{
    std::ifstream file(util::StringUtils::ToWide(path), std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return {};
    const size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> bytes(size);
    file.read(static_cast<char*>(static_cast<void*>(bytes.data())), static_cast<std::streamsize>(size));
    return bytes;
}

bool DX12Shader::Init(const std::string& path)
{
    m_path = path;
    const std::string base = CompiledBase(path);
    if (path.ends_with(".cs.hlsl")) {
        const std::string csoPath = base + ".cso";
        const bool stale = shader_dependency::IsBinaryStale(path, csoPath);
        m_computeBlob = stale ? std::vector<uint8_t>{} : LoadBinary(csoPath);
        const bool loadedCso = !m_computeBlob.empty();
        if (m_computeBlob.empty()) {
            FBZZ_LOG_WARN("DX12Shader: CSO欠落/期限切れ、HLSLからコンパイル: %s", path.c_str());
            m_computeBlob = CompileShader(path, "CSMain", "cs_6_8", csoPath);
        }
        if (!m_computeBlob.empty())
            FBZZ_LOG_DEBUG("DX12Shader: CS準備完了 [%s] source=%s bytes=%zu",
                          path.c_str(), loadedCso ? "compiled_dx12" : "runtime", m_computeBlob.size());
        return !m_computeBlob.empty();
    }
    const std::string vsPath = base + ".vs.cso";
    const std::string psPath = base + ".ps.cso";
    const bool vsStale = shader_dependency::IsBinaryStale(path, vsPath);
    const bool psStale = shader_dependency::IsBinaryStale(path, psPath);
    m_vertexBlob = vsStale ? std::vector<uint8_t>{} : LoadBinary(vsPath);
    m_pixelBlob = psStale ? std::vector<uint8_t>{} : LoadBinary(psPath);
    const bool loadedCso = !m_vertexBlob.empty() && !m_pixelBlob.empty();
    if (m_vertexBlob.empty())
        m_vertexBlob = CompileShader(path, "VSMain", "vs_6_8", vsPath);
    if (m_pixelBlob.empty())
        m_pixelBlob = CompileShader(path, "PSMain", "ps_6_8", psPath);
    if (m_vertexBlob.empty() || m_pixelBlob.empty()) return false;
    FBZZ_LOG_DEBUG("DX12Shader: VS/PS準備完了 [%s] source=%s vs=%zu ps=%zu",
                  path.c_str(), loadedCso ? "compiled_dx12" : "runtime",
                  m_vertexBlob.size(), m_pixelBlob.size());
    // PS バイトコードから MaterialConstants とテクスチャバインドを取得する。
    // WHY: Material Inspector・SyncMaterial (Forward/Deferred)・AssetBrowser サムネイルは
    //      ShaderDescriptor を頼りに Material CB を構築する。ここを省くとマテリアル値が
    //      一切反映されず、Material プレビューも生成不能になる (DX11 と同じ処理が必須)。
    m_descriptor = BuildDescriptor(m_pixelBlob);
    if (!ReflectVertexInput()) {
        FBZZ_LOG_ERROR("DX12Shader: Vertex Input Reflection失敗 [%s]", path.c_str());
        return false;
    }
    FBZZ_LOG_DEBUG("DX12Shader: Vertex Input Reflection完了 [%s] elements=%zu",
                  path.c_str(), m_inputElements.size());
    return true;
}

bool DX12Shader::ReflectVertexInput()
{
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection =
        CreateShaderReflection(m_vertexBlob);
    if (!reflection) return false;
    D3D12_SHADER_DESC shaderDesc{};
    if (FAILED(reflection->GetDesc(&shaderDesc)))
        return false;
    m_semanticNames.reserve(shaderDesc.InputParameters);
    m_inputElements.reserve(shaderDesc.InputParameters);
    UINT byteOffset = 0;
    for (UINT index = 0; index < shaderDesc.InputParameters; ++index) {
        D3D12_SIGNATURE_PARAMETER_DESC parameter{};
        if (FAILED(reflection->GetInputParameterDesc(index, &parameter))
            || parameter.SystemValueType != D3D_NAME_UNDEFINED)
            continue;
        m_semanticNames.emplace_back(parameter.SemanticName);
        D3D12_INPUT_ELEMENT_DESC element{};
        element.SemanticName = m_semanticNames.back().c_str();
        element.SemanticIndex = parameter.SemanticIndex;
        element.InputSlot = 0;
        element.AlignedByteOffset = byteOffset;
        element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        const bool isUInt = parameter.ComponentType == D3D_REGISTER_COMPONENT_UINT32;
        if (parameter.Mask <= 0x1) { element.Format = isUInt ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT; byteOffset += 4; }
        else if (parameter.Mask <= 0x3) { element.Format = isUInt ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32_FLOAT; byteOffset += 8; }
        else if (parameter.Mask <= 0x7) { element.Format = isUInt ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32_FLOAT; byteOffset += 12; }
        else { element.Format = isUInt ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT; byteOffset += 16; }
        m_inputElements.push_back(element);
        // 診断: リフレクションが決めた各要素のオフセットを残す。実バッファのレイアウトと
        // ずれていた場合 (パディングや宣言順の差)、ここのログが照合の起点になる。
        FBZZ_LOG_DEBUG("DX12Shader:   input[%zu] %s%u offset=%u mask=0x%X %s",
                      m_inputElements.size() - 1, parameter.SemanticName, parameter.SemanticIndex,
                      element.AlignedByteOffset, parameter.Mask, isUInt ? "uint" : "float");
    }
    m_reflectedStride = byteOffset;
    return true;
}

D3D12_SHADER_BYTECODE DX12Shader::GetVertexBytecode() const
{
    return {m_vertexBlob.data(), m_vertexBlob.size()};
}

D3D12_SHADER_BYTECODE DX12Shader::GetPixelBytecode() const
{
    return {m_pixelBlob.data(), m_pixelBlob.size()};
}

D3D12_SHADER_BYTECODE DX12Shader::GetComputeBytecode() const
{
    return {m_computeBlob.data(), m_computeBlob.size()};
}

} // namespace fbzz::renderer
