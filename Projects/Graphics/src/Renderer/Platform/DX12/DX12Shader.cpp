/// @file    DX12Shader.cpp
/// @brief   DXC / SM 6.8 による DXIL コンパイルと DXBC / DXIL 両対応リフレクション。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12Shader.hpp"

#include <Core/Logger.hpp>
#include "GraphicsRuntimeContract.hpp"
#include <Graphics/Renderer/ShaderCompileDiagnostics.hpp>
#include <Graphics/Renderer/ShaderDependencyTracker.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Core/Util/StringUtils.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cctype>
#include <cwctype>
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <wrl/client.h>

namespace fbzz::renderer {

namespace {

uint64_t NextShaderCacheIdentity()
{
    static std::atomic<uint64_t> next{1};
    const uint64_t identity = next.fetch_add(1, std::memory_order_relaxed);
    assert(identity != 0);
    return identity;
}

/// @note 相対Assetsパスを実ファイルへ解決する。CWD探索とSDKのEngine assetルート探索は
/// @note ResolveShaderFilePath に集約している。
std::wstring ResolveShaderPath(const std::string& path)
{
    return ResolveShaderFilePath(util::StringUtils::ToWide(path)).wstring();
}

/// @note 配布物の欠落を PATH / 環境変数で補わず、EXE 基準の検証済み一式だけをロードする。
/// @see https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR.
DxcCreateInstanceProc GetDxcCreateInstance()
{
    static HMODULE module = [] {
        wchar_t hostPath[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, hostPath, static_cast<DWORD>(std::size(hostPath)));
        if (length == 0 || length >= std::size(hostPath)) return static_cast<HMODULE>(nullptr);
        const auto dllPath = std::filesystem::path(hostPath).parent_path() / "dxcompiler.dll";
        HMODULE loaded = LoadLibraryExW(dllPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        wchar_t actualPath[32768]{};
        if (loaded && (!GetModuleFileNameW(loaded, actualPath, static_cast<DWORD>(std::size(actualPath)))
            || _wcsicmp(actualPath, dllPath.c_str()) != 0)) {
            FBZZ_LOG_ERROR("DX12Shader: another DXC compiler is already loaded: %ls (expected %ls)", actualPath, dllPath.c_str());
            FreeLibrary(loaded);
            return static_cast<HMODULE>(nullptr);
        }
        if (loaded) FBZZ_LOG_INFO("DX12Shader: actual DXC compiler=%ls package=%s", actualPath, runtimecontract::DXC_PACKAGE_VERSION);
        else FBZZ_LOG_ERROR("DX12Shader: DXC load failed: %s (error=%lu)", dllPath.string().c_str(), GetLastError());
        return loaded;
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

/// @note 初期導入・DXC 一式更新時は時刻が新しい CSO でも再生成する。
bool HasCompilerSignature(const std::string& csoPath, const char* entry, const char* profile)
{
    std::ifstream stamp(ResolveShaderPath(csoPath) + L".compiler", std::ios::binary);
    std::string signature;
    std::getline(stamp, signature);
    const std::string expected = std::string(runtimecontract::DXC_SIGNATURE) + "|" + entry + "|"
        + profile + "|FBZZ_BACKEND_DX12=1|HV2021";
    return signature == expected;
}

/// @note DXBC は D3DReflect、DXIL は DXC の container reflection で同じ D3D12 API に正規化する。
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

/// @note HLSLをDX12定義付きで SM 6.8 DXIL へコンパイルし、次回起動用に保存する。
std::vector<uint8_t> CompileShader(
    const std::string& path, const char* entryPoint, const char* target,
    const std::string& csoSavePath)
{
    ClearShaderCompileDiagnosticsFor(path, entryPoint, target);
    const std::wstring resolved = ResolveShaderPath(path);
    const auto shaderRoot = shader_dependency::FindShaderRoot(std::filesystem::path(resolved));
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    if (!CreateDxcServices(utils, &compiler)) {
        const std::string message =
            "検証済み dxcompiler.dll / dxil.dll 一式を実行ファイルの隣へ配置してください";
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

    /// @note 再コンパイル結果を保存し、次回起動時のコンパイルを避ける。
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
            binary.flush();
            if (binary.good()) {
                std::ofstream stamp(output.wstring() + L".compiler", std::ios::binary);
                stamp << runtimecontract::DXC_SIGNATURE << '|' << entryPoint << '|' << target
                    << "|FBZZ_BACKEND_DX12=1|HV2021\n";
#if defined(_DEBUG)
                stamp << "-Zi -Qembed_debug -Od";
#else
                stamp << "-O3";
#endif
            }
            FBZZ_LOG_INFO("DX12Shader: CSOを更新しました: %s", csoSavePath.c_str());
        } else {
            FBZZ_LOG_WARN("DX12Shader: CSOを保存できません: %s", csoSavePath.c_str());
        }
    }
    const auto* begin = static_cast<const uint8_t*>(code->GetBufferPointer());
    return {begin, begin + code->GetBufferSize()};
}

} /// @note namespace

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

    /// @note 既存アセットの互換エイリアスを DX11 と同じ CSO 名へ正規化する。
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

namespace {

/// @note 添字フィールド名 → マテリアルのテクスチャ枠番号。該当しなければ UINT32_MAX。
uint32_t MaterialTextureSlotOf(std::string_view field)
{
    for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot)
        if (field == kMaterialTextureSlots[slot].field) return slot;
    return UINT32_MAX;
}


/// @note legacy cbuffer の配列は 16 byte 境界、行列は格納方向のベクトルごとに 16 byte 境界。
/// @see https://github.com/microsoft/DirectXShaderCompiler/wiki/Buffer-Packing Legacy layout
uint32_t ReflectedExtent(ID3D12ShaderReflectionType* type, bool array = true)
{
    D3D12_SHADER_TYPE_DESC desc{};
    if (!type || FAILED(type->GetDesc(&desc))) return 0;
    uint32_t size = 0;
    if (desc.Class == D3D_SVC_STRUCT) {
        for (UINT i = 0; i < desc.Members; ++i) {
            auto* member = type->GetMemberTypeByIndex(i);
            D3D12_SHADER_TYPE_DESC child{};
            if (member && SUCCEEDED(member->GetDesc(&child)))
                size = (std::max)(size, child.Offset + ReflectedExtent(member));
        }
    } else {
        const uint32_t scalarSize = desc.Type == D3D_SVT_DOUBLE
            || desc.Type == D3D_SVT_INT64 || desc.Type == D3D_SVT_UINT64 ? 8u : 4u;
        if (desc.Class == D3D_SVC_MATRIX_ROWS || desc.Class == D3D_SVC_MATRIX_COLUMNS) {
            const bool rowMajor = desc.Class == D3D_SVC_MATRIX_ROWS;
            const uint32_t major = rowMajor ? desc.Rows : desc.Columns;
            const uint32_t minor = rowMajor ? desc.Columns : desc.Rows;
            const uint32_t stride = (minor * scalarSize + 15u) & ~15u;
            size = major ? (major - 1u) * stride + minor * scalarSize : 0;
        } else size = desc.Rows * desc.Columns * scalarSize;
    }
    if (array && desc.Elements)
        size = (desc.Elements - 1u) * ((size + 15u) & ~15u) + size;
    return size;
}

void AppendReflectedVariables(ID3D12ShaderReflectionType* type, const std::string& name,
                              uint32_t offset, uint32_t size, std::vector<ShaderVarDesc>& variables)
{
    D3D12_SHADER_TYPE_DESC desc{};
    if (!type || FAILED(type->GetDesc(&desc))) return;
    if (desc.Class == D3D_SVC_STRUCT) {
        const uint32_t stride = (ReflectedExtent(type, false) + 15u) & ~15u;
        for (uint32_t element = 0; element < (desc.Elements ? desc.Elements : 1u); ++element) {
            const std::string prefix = name + (desc.Elements ? "[" + std::to_string(element) + "]" : "");
            for (UINT i = 0; i < desc.Members; ++i) {
                auto* member = type->GetMemberTypeByIndex(i);
                const char* memberName = type->GetMemberTypeName(i);
                D3D12_SHADER_TYPE_DESC child{};
                if (!memberName || !member || FAILED(member->GetDesc(&child))) continue;
                AppendReflectedVariables(member, prefix + "." + memberName,
                    offset + element * stride + child.Offset, ReflectedExtent(member), variables);
            }
        }
        return;
    }
    ShaderVarDesc value;
    value.name = name;
    value.offset = offset;
    value.size = size;
    value.rows = static_cast<uint8_t>(desc.Rows);
    value.columns = static_cast<uint8_t>(desc.Columns);
    value.elements = desc.Elements;
    value.arrayStride = desc.Elements ? (ReflectedExtent(type, false) + 15u) & ~15u : 0u;
    value.rowMajor = desc.Class == D3D_SVC_MATRIX_ROWS;
    switch (desc.Class) {
    case D3D_SVC_SCALAR: value.varClass = ShaderVarClass::Scalar; break;
    case D3D_SVC_VECTOR: value.varClass = ShaderVarClass::Vector; break;
    case D3D_SVC_MATRIX_ROWS:
    case D3D_SVC_MATRIX_COLUMNS: value.varClass = ShaderVarClass::Matrix; break;
    default: value.varClass = ShaderVarClass::UNSUPPORTED; break;
    }
    switch (desc.Type) {
    case D3D_SVT_FLOAT: value.varType = ShaderVarType::Float; break;
    case D3D_SVT_INT: value.varType = ShaderVarType::Int; break;
    case D3D_SVT_UINT: value.varType = ShaderVarType::UInt; break;
    case D3D_SVT_BOOL: value.varType = ShaderVarType::Bool; break;
    default: value.varType = ShaderVarType::UNSUPPORTED; break;
    }
    if (!value.IsWritable()) {
        value.unsupportedReason = "Only 32-bit float/int/uint/bool scalar, vector and matrix leaves are writable";
        FBZZ_LOG_WARN("Shader reflection: %s: %s", name.c_str(), value.unsupportedReason.c_str());
    }
    variables.push_back(std::move(value));
}

} /// @note namespace

ShaderDescriptor DX12Shader::BuildDescriptor(const std::vector<uint8_t>& psBlob)
{
    /// @note DXBC / DXIL の PS reflection を共通化し、DX11 と同じ descriptor を構築する。
    /// @note Inspector / SyncMaterial / サムネイルへバックエンド差を漏らさないため。
    ShaderDescriptor desc;

    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> refl = CreateShaderReflection(psBlob);
    if (!refl) return desc;

    /// @name MaterialConstants (b2) から編集可能変数を列挙
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

                /// @note D3D Reflection は最適化済みシェーダーや匿名パディング相当の変数で Name が null になりうる。
                /// @note std::string_view(nullptr) は MSVC STL の strlen 経路でクラッシュするため、
                /// @note null は編集対象外として捨てる。
                std::string_view n = vDesc.Name;
                /// @note パディング変数はスキップ
                if (n.starts_with("_")) continue;
                /// @note textureMask は Inspector に出さないが offset を記録する
                if (n == "textureMask") {
                    desc.textureMaskOffset = vDesc.StartOffset;
                    continue;
                }
                /// @note テクスチャ添字フィールドは «編集可能変数» ではなくテクスチャ枠。
                /// @note Inspector の数値欄に uint が並ぶのを避け、代わりにテクスチャ枠として出す。
                if (const uint32_t slot = MaterialTextureSlotOf(n); slot != UINT32_MAX) {
                    ShaderTexBindDesc bind;
                    bind.name = vDesc.Name;
                    bind.slot = slot;
                    bind.constantOffset = vDesc.StartOffset;
                    desc.textures.push_back(std::move(bind));
                    continue;
                }

                AppendReflectedVariables(var->GetType(), vDesc.Name, vDesc.StartOffset, vDesc.Size, desc.vars);
            }
        }
    }

    /// @name PostProcConstants (b5) から custom* 変数を列挙
    /// @note 共通 PostProcess Inspector が実際にシェーダーで使用される入力だけを表示するため。
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

                AppendReflectedVariables(var->GetType(), vDesc.Name, vDesc.StartOffset, vDesc.Size, desc.postProcessVars);
            }
        }
    }

    /// @note かつてここで D3D_SIT_TEXTURE の束縛 (t0〜t4) を列挙していた。bindless では
    /// @note ResourceDescriptorHeap から引くテクスチャが DXIL に束縛情報を残さないため、
    /// @note この経路は «どのシェーダーでもテクスチャ 0 件» になる。枠の正本は
    /// @note MaterialConstants の添字フィールドへ移した (上のループ)。
    std::sort(desc.textures.begin(), desc.textures.end(),
        [](const auto& a, const auto& b) { return a.slot < b.slot; });

    return desc;
}

std::vector<uint8_t> DX12Shader::LoadBinary(const std::string& path)
{
    /// @note CSO も HLSL と同じ解決規則に通す (SDK の共有 asset だけが実体を持つ構成があるため)。
    std::ifstream file(ResolveShaderPath(path), std::ios::binary | std::ios::ate);
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
    m_cacheIdentity = NextShaderCacheIdentity();
    m_path = path;
    const std::string base = CompiledBase(path);
    if (path.ends_with(".cs.hlsl")) {
        const std::string csoPath = base + ".cso";
        const bool stale = shader_dependency::IsBinaryStale(path, csoPath)
            || !HasCompilerSignature(csoPath, "CSMain", "cs_6_8");
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
    const bool vsStale = shader_dependency::IsBinaryStale(path, vsPath)
        || !HasCompilerSignature(vsPath, "VSMain", "vs_6_8");
    const bool psStale = shader_dependency::IsBinaryStale(path, psPath)
        || !HasCompilerSignature(psPath, "PSMain", "ps_6_8");
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
    /// @note PS バイトコードから MaterialConstants とテクスチャバインドを取得する。Material Inspector・
    /// @note SyncMaterial・AssetBrowser サムネイルは ShaderDescriptor を頼りに Material CB を構築するため、
    /// @note 省くとマテリアル値が反映されずプレビューも生成できない (DX11 と同じ処理が必須)。
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
        /// @note 診断: リフレクションが決めた各要素のオフセットを残す。実バッファのレイアウトと
        /// @note ずれていた場合 (パディングや宣言順の差)、ここのログが照合の起点になる。
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

} /// @note namespace fbzz::renderer
