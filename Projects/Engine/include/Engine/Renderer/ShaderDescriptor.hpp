/// @file    ShaderDescriptor.hpp
/// @brief   PS バイトコードをリフレクションして得られるシェーダーメタ情報。
/// @author  Hasegawa Jin
/// @date    2026-05-30
/// @note MaterialConstants / PostProcConstants の変数レイアウトとテクスチャバインドを保持する。
/// @note 各バックエンドの IShader::Init() が構築し、Material・PostProcess Inspector と SyncMaterial が参照する。
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace fbzz::renderer {

enum class ShaderVarClass { Scalar, Vector, Matrix, UNSUPPORTED };
enum class ShaderVarType  { Float, Int, UInt, Bool, UNSUPPORTED };

struct ShaderVarDesc {
    std::string    name;
    uint32_t       offset = 0;
    uint32_t       size = 0;
    uint8_t        rows = 1;
    uint8_t        columns = 1;
    ShaderVarClass varClass = ShaderVarClass::Scalar;
    ShaderVarType  varType = ShaderVarType::Float;
    /// @note 非配列は 0。配列・行列のパディングは保存する値列に含めない。
    uint32_t elements = 0;
    uint32_t arrayStride = 0;
    uint32_t matrixStride = 16;
    bool rowMajor = false;
    std::string unsupportedReason;

    [[nodiscard]] uint32_t ValueCount() const
    {
        return static_cast<uint32_t>(rows) * columns * (elements ? elements : 1u);
    }
    [[nodiscard]] bool IsWritable() const
    {
        return unsupportedReason.empty() && varType != ShaderVarType::UNSUPPORTED
            && varClass != ShaderVarClass::UNSUPPORTED && rows > 0 && rows <= 4
            && columns > 0 && columns <= 4;
    }
    /// @note CPU 側は配列要素順・行優先。GPU の行列配置へここで変換する。
    /// @see https://github.com/microsoft/DirectXShaderCompiler/wiki/Buffer-Packing Legacy cbuffer layout
    [[nodiscard]] uint64_t ValueOffset(uint32_t index) const
    {
        const uint32_t componentCount = static_cast<uint32_t>(rows) * columns;
        const uint32_t element = index / componentCount;
        const uint32_t component = index % componentCount;
        if (varClass != ShaderVarClass::Matrix)
            return static_cast<uint64_t>(offset) + static_cast<uint64_t>(element) * arrayStride + component * 4u;
        const uint32_t row = component / columns;
        const uint32_t column = component % columns;
        return static_cast<uint64_t>(offset) + static_cast<uint64_t>(element) * arrayStride
            + (rowMajor ? row : column) * matrixStride + (rowMajor ? column : row) * 4u;
    }
};

/// @note マテリアルが差せるテクスチャ枠 1 つ。
/// @note bindless 移行前はレジスタ束縛 (t0〜t4) のリフレクションから作っていたが、
/// @note ResourceDescriptorHeap から引くテクスチャは DXIL に束縛情報を残さない。
/// @note いまは MaterialConstants の添字フィールド (texAlbedoIndex 等) が正本で、
/// @note Material::Upload がそこへ実際の bindless 添字を書き込む。
/// @see  Docs/design/bindless.md
struct ShaderTexBindDesc {
    std::string name;            ///< 添字フィールド名 ("texAlbedoIndex")
    uint32_t    slot;            ///< 0=albedo .. 4=ao、5〜7=カスタム汎用枠
    /// @note MaterialConstants 内での添字フィールドの位置 [byte]。
    /// @note UINT32_MAX は «枠は宣言されているが書き込み先が無い» = 異常。
    uint32_t    constantOffset = UINT32_MAX;
};

/// @note マテリアルのテクスチャ枠の正本。添字フィールド名 → スロット番号 → .mat のキー。
/// @note LAYOUT: 3 つは必ず同じ並びであること。
/// @note - HLSL 側: MaterialConstants の `uint <field>;` (Common/MaterialTextures.hlsli)
/// @note - Editor : Inspector が .mat の [textures] キーとして使う名前
/// @note HLSL の変数名・Inspector の kCanonicalSlots・.mat のキーが別ファイルに分かれていると、
/// @note ずれても «別のテクスチャが貼られる» としてしか現れない。1 か所へ置いてずれようがなくする。
struct MaterialTextureSlot {
    const char* field;  ///< MaterialConstants の添字フィールド名
    const char* key;    ///< .mat の [textures] キー
};
inline constexpr MaterialTextureSlot kMaterialTextureSlots[] = {
    { "texAlbedoIndex",   "albedo"   },
    { "texNormalIndex",   "normal"   },
    { "texMetallicIndex", "metallic" },
    { "texEmissiveIndex", "emissive" },
    { "texAOIndex",       "ao"       },
    { "tex5Index",        "tex5"     },
    { "tex6Index",        "tex6"     },
    { "tex7Index",        "tex7"     },
};
inline constexpr uint32_t kMaterialTextureSlotCount =
    static_cast<uint32_t>(sizeof(kMaterialTextureSlots) / sizeof(kMaterialTextureSlots[0]));

struct ShaderDescriptor {
    std::vector<ShaderVarDesc>     vars;               ///< textureMask・パディングを除いた編集可能変数
    std::vector<ShaderVarDesc>     postProcessVars;    ///< PostProcConstants (b5) の custom* 編集変数
    std::vector<ShaderTexBindDesc> textures;            ///< slot 昇順
    uint32_t                       cbufferSize      = 0;
    uint32_t                       textureMaskOffset = UINT32_MAX; ///< UINT32_MAX = 未存在

    bool IsValid() const { return cbufferSize > 0; }

    const ShaderVarDesc* FindVar(std::string_view name) const
    {
        for (auto& v : vars)
            if (v.name == name) return &v;
        return nullptr;
    }

    /// @note PostProcConstants のカスタムエフェクト用変数を名前で検索する。
    const ShaderVarDesc* FindPostProcessVar(std::string_view name) const
    {
        for (const auto& v : postProcessVars)
            if (v.name == name) return &v;
        return nullptr;
    }
};

} // namespace fbzz::renderer
