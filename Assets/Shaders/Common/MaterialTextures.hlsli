// FBZZ Engine
// MaterialTextures.hlsli | Common
// マテリアルが差すテクスチャの bindless 添字と、その参照マクロ
//
// LAYOUT: 並びは Engine/Renderer/ShaderDescriptor.hpp の kMaterialTextureSlots が正本。
//         フィールド名が食い違うと «Inspector に枠が出ない» か «別の枠へ差さる» になる。
//
// WHY cbuffer に添字を置くか: bindless のテクスチャは DXIL に束縛情報を残さないため、
//     レジスタのリフレクションからは «このシェーダーが何枚テクスチャを使うか» が分からない。
//     cbuffer の変数はリフレクションに残るので、添字フィールドを枠の宣言そのものにする。
//     値は Material::Upload が毎フレーム書き込む。
//
// 使い方:
//   cbuffer MaterialConstants : register(CB_MATERIAL)
//   {
//       float4 albedo;
//       ...
//       FBZZ_MATERIAL_TEXTURE_INDICES   // 末尾に置く
//   };
//   FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
//
// @see Docs/design/bindless.md
#ifndef FBZZ_COMMON_MATERIAL_TEXTURES_HLSLI
#define FBZZ_COMMON_MATERIAL_TEXTURES_HLSLI

#include "Common/BindlessIndices.hlsli"

// MaterialConstants の末尾へ置く添字フィールド一式。
//
// WHY 使わない枠も宣言するか: 宣言した枠だけが Inspector に出る。albedo しか使わない
//     シェーダーで 8 個すべて宣言すると、差せない枠が並んで «差したのに効かない» になる。
//     そのため «全部入り» ではなく、必要な枠だけを個別に宣言するマクロも用意してある。
#define FBZZ_MATERIAL_TEX_ALBEDO    uint texAlbedoIndex;
#define FBZZ_MATERIAL_TEX_NORMAL    uint texNormalIndex;
#define FBZZ_MATERIAL_TEX_METALLIC  uint texMetallicIndex;
#define FBZZ_MATERIAL_TEX_EMISSIVE  uint texEmissiveIndex;
#define FBZZ_MATERIAL_TEX_AO        uint texAOIndex;
#define FBZZ_MATERIAL_TEX_5         uint tex5Index;
#define FBZZ_MATERIAL_TEX_6         uint tex6Index;
#define FBZZ_MATERIAL_TEX_7         uint tex7Index;

/// PBR 系がそのまま使う 5 枠。
#define FBZZ_MATERIAL_TEXTURE_INDICES \
    FBZZ_MATERIAL_TEX_ALBEDO   \
    FBZZ_MATERIAL_TEX_NORMAL   \
    FBZZ_MATERIAL_TEX_METALLIC \
    FBZZ_MATERIAL_TEX_EMISSIVE \
    FBZZ_MATERIAL_TEX_AO

/// 添字から Texture2D を取り出す。宣言 1 行で、参照側は従来どおり name.Sample(...)。
/// @note 添字が無効 (テクスチャ未設定) のときは ResourceDescriptorHeap を引かないこと。
///       シェーダーは textureMask で分岐しているので、既存の判定がそのまま使える。
#define FBZZ_MATERIAL_TEX(name, indexField) \
    static Texture2D name = ResourceDescriptorHeap[NonUniformResourceIndex(indexField)]

#endif // FBZZ_COMMON_MATERIAL_TEXTURES_HLSLI
