// FBZZ Engine
// DX11Shader.cpp | fbzz::renderer
// DX11 頂点・ピクセルシェーダーと InputLayout の管理
#include "DX11Shader.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/HResult.hpp>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#include <fstream>
#include <vector>
#include <string>

namespace fbzz::renderer
{

// "assets/shaders/Mesh.hlsl"         → "assets/shaders/compiled/Mesh"
// "assets/shaders/Material/PBR.hlsl" → "assets/shaders/compiled/Material.PBR"
// shaders/ アンカーより後の相対パス (拡張子除く) の区切り文字を '.' に変換して
// assets/shaders/compiled/ 以下に配置されたプリコンパイル済み CSO を指す。
std::string DX11Shader::CompiledBase(const std::string& path)
{
    const std::string anchor = "shaders/";
    size_t a = path.find(anchor);

    if (a == std::string::npos)
    {
        // フォールバック: 旧ロジック (shaders/ が見つからない場合)
        size_t slash = path.find_last_of("/\\");
        size_t dot   = path.find_last_of('.');
        std::string dir  = (slash != std::string::npos) ? path.substr(0, slash + 1) : "";
        std::string stem = path.substr(slash + 1, dot - slash - 1);
        return dir + "compiled/" + stem;
    }

    std::string base = path.substr(0, a + anchor.size()); // "assets/shaders/"
    std::string rel  = path.substr(a + anchor.size());    // "Material/PBR.hlsl"

    // 拡張子を除去
    size_t dot = rel.find_last_of('.');
    if (dot != std::string::npos) rel = rel.substr(0, dot); // "Material/PBR"

    // パス区切り文字を '.' に変換 → "Material.PBR"
    for (char& c : rel)
        if (c == '/' || c == '\\') c = '.';

    return base + "compiled/" + rel; // "assets/shaders/compiled/Material.PBR"
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

    const std::string base = CompiledBase(path);

    // .cs.hlsl は Compute Shader — VS/PS の代わりに単一 .cs.cso をロードする
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
    // Input Layout — VS バイトコードをリフレクションして頂点レイアウトを自動構築する
    //
    // 【なぜリフレクションを使うのか】
    //   CreateInputLayout には、頂点バッファの各要素が HLSL 側のどのセマンティクスに
    //   対応するかを示す D3D11_INPUT_ELEMENT_DESC の配列が必要になる。
    //   この配列を手書きでハードコードすると、シェーダーを追加するたびに C++ 側も
    //   修正しなければならず、見落としが起きやすい。
    //   D3DReflect を使うと VS バイトコードの入力シグネチャから HLSL 側の宣言を
    //   直接読み取れるため、C++ を変更せずに任意の頂点フォーマットに対応できる。
    //
    // 【D3DReflect の依存関係】
    //   D3DReflect は d3dcompiler.lib (→ d3dcompiler_47.dll) から提供される。
    //   ランタイムシェーダーコンパイル (D3DCompile) と同じ DLL だが、
    //   リフレクションはコンパイル済み CSO を読むだけなので実行時の負荷は軽い。
    //   IID は __uuidof で取得する (dxguid.lib リンクが不要になる)。
    //
    // 【Mask からコンポーネント数を判定する仕組み】
    //   paramDesc.Mask は VS 入力レジスタの使用ビットを示すビットフィールド。
    //   float4 なら 0b1111 (=0xF)、float3 なら 0b0111 (=0x7)、
    //   float2 なら 0b0011 (=0x3)、float1 なら 0b0001 (=0x1)。
    //   このビット数からバイト幅と DXGI フォーマットを一意に決定できる。
    //   現状は float 型のみに対応 (int / uint はエンジンの頂点フォーマットに存在しない)。
    //
    // 【semanticNames を別途保持する理由】
    //   D3D11_INPUT_ELEMENT_DESC::SemanticName は const char* (生ポインタ) であり、
    //   CreateInputLayout の呼び出しが完了するまでその文字列が生存していなければ
    //   ならない。paramDesc.SemanticName は pReflector 内部のバッファを指しているが、
    //   pReflector のスコープを超えた後も参照されうるため、std::string にコピーして
    //   保持する。emplace_back による再アロケーションで c_str() ポインタが無効化
    //   されるのを防ぐため、ループ前に reserve() で容量を確保する。
    // -------------------------------------------------------------------------
    {
        // --- (1) リフレクターを取得 ---
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> pReflector;
        HRESULT hrRefl = D3DReflect(vsBlob.data(), vsBlob.size(),
                                     __uuidof(ID3D11ShaderReflection),
                                     reinterpret_cast<void**>(pReflector.GetAddressOf()));
        if (FAILED(hrRefl))
        {
            FBZZ_LOG_ERROR("頂点シェーダーリフレクション失敗: %s", vsPath.c_str());
            return false;
        }

        // --- (2) 入力パラメーター数を取得 ---
        D3D11_SHADER_DESC shaderDesc = {};
        pReflector->GetDesc(&shaderDesc);

        std::vector<D3D11_INPUT_ELEMENT_DESC> inputElements;
        std::vector<std::string>              semanticNames;
        semanticNames.reserve(shaderDesc.InputParameters); // c_str() 安定化のため必須
        UINT byteOffset = 0;

        // --- (3) 各入力パラメーターを D3D11_INPUT_ELEMENT_DESC に変換 ---
        for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
        {
            D3D11_SIGNATURE_PARAMETER_DESC paramDesc = {};
            pReflector->GetInputParameterDesc(i, &paramDesc);

            // SV_VertexID や SV_InstanceID など D3D 組み込みの系統値はスキップ。
            // これらは頂点バッファから供給されず IA が自動生成するため
            // InputLayout に含めると CreateInputLayout が E_INVALIDARG を返す。
            if (paramDesc.SystemValueType != D3D_NAME_UNDEFINED) continue;

            // SemanticName を std::string にコピーして生存期間を延ばす
            semanticNames.emplace_back(paramDesc.SemanticName);

            D3D11_INPUT_ELEMENT_DESC elem   = {};
            elem.SemanticName               = semanticNames.back().c_str();
            elem.SemanticIndex              = paramDesc.SemanticIndex;
            elem.InputSlot                  = 0;  // 単一頂点バッファのみ (マルチストリーム未対応)
            elem.AlignedByteOffset          = byteOffset;
            elem.InputSlotClass             = D3D11_INPUT_PER_VERTEX_DATA;
            elem.InstanceDataStepRate       = 0;

            // Mask のビット数 → コンポーネント数 → DXGI フォーマット & バイト幅
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

        // --- (4) InputLayout オブジェクトを生成 ---
        // SV_VertexID のみ使う VS (Composite 等) は inputElements が空になる。
        // NumElements=0 で CreateInputLayout を呼ぶと E_INVALIDARG になるため
        // スキップして m_inputLayout を null のままにする。
        // IASetInputLayout(nullptr) は DX11 で有効 (頂点入力なし)。
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

    // InputLayout を IA (Input Assembler) ステージにバインドする。
    // IASetVertexBuffers より先に呼んでも後に呼んでも動作は同じだが、
    // 可読性のため Shader → InputLayout → VertexBuffer の順に揃えている。
    context->IASetInputLayout(m_inputLayout.Get());
}

} // namespace fbzz::renderer
