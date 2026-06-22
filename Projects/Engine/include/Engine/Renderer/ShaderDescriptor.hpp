// FBZZ Engine
// ShaderDescriptor.hpp | fbzz::renderer
// PS バイトコードをリフレクションして得られるシェーダーメタ情報。
// MaterialConstants / PostProcConstants の変数レイアウトとテクスチャバインドを保持する。
// DX11Shader::Init() が構築し、Material・PostProcess Inspector と SyncMaterial が参照する。
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace fbzz::renderer {

enum class ShaderVarClass { Scalar, Vector, Matrix };
enum class ShaderVarType  { Float, Int, UInt, Bool };

struct ShaderVarDesc {
    std::string    name;
    uint32_t       offset;   // CB_MATERIAL 内のバイトオフセット
    uint32_t       size;     // バイトサイズ (変数全体)
    uint8_t        rows;     // scalar/vector は 1
    uint8_t        columns;  // コンポーネント数 (1=float, 2=float2, 3=float3, 4=float4)
    ShaderVarClass varClass;
    ShaderVarType  varType;
};

struct ShaderTexBindDesc {
    std::string name;
    uint32_t    slot; // 0=t0 (albedo) .. 4=t4 (ao)
};

struct ShaderDescriptor {
    std::vector<ShaderVarDesc>     vars;               // textureMask・パディングを除いた編集可能変数
    std::vector<ShaderVarDesc>     postProcessVars;    // PostProcConstants (b5) の custom* 編集変数
    std::vector<ShaderTexBindDesc> textures;            // slot 昇順
    uint32_t                       cbufferSize      = 0;
    uint32_t                       textureMaskOffset = UINT32_MAX; // UINT32_MAX = 未存在

    bool IsValid() const { return cbufferSize > 0; }

    const ShaderVarDesc* FindVar(std::string_view name) const
    {
        for (auto& v : vars)
            if (v.name == name) return &v;
        return nullptr;
    }

    // PostProcConstants のカスタムエフェクト用変数を名前で検索する。
    const ShaderVarDesc* FindPostProcessVar(std::string_view name) const
    {
        for (const auto& v : postProcessVars)
            if (v.name == name) return &v;
        return nullptr;
    }
};

} // namespace fbzz::renderer
