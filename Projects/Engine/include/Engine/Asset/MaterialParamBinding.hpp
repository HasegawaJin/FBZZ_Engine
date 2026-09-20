/// @file    MaterialParamBinding.hpp
/// @brief   .mat の params / textures を、シェーダーリフレクションに従って生バイト列へ束縛する
/// @author  Hasegawa Jin
/// @date    2026-08-22
/// @note 描画パスから切り出す理由: この束縛は元々 GeometryPassHelpers.cpp の無名名前空間にあり、
/// @note メッシュ描画からしか呼べなかった。UI のように「同じ .mat 形式を、違う頂点入力とパスで
/// @note 描く」側が増えるたびに写経することになり、写した先だけが追従しなくなるため、
/// @note 束縛の規則を 1 箇所に置きパスは結果を受け取るだけにした。
/// @note ShaderDescriptor 経由の理由: .mat は「シェーダー変数名 → float 列」で値を持ち、
/// @note バイトオフセットを持たない。オフセットはシェーダーをコンパイルして初めて決まるため、
/// @note 保存時に焼き付けるとシェーダーを 1 行足しただけで全 .mat が壊れる。名前で引き直す。
#pragma once

#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <span>

namespace fbzz::asset {

/// @note 値列は行優先でパディングなし。型・範囲・要素数・転送先を検証し、失敗時は未変更。
[[nodiscard]] bool ValidateMaterialValues(const renderer::ShaderVarDesc& variable,
                                           std::span<const double> values);
[[nodiscard]] bool WriteMaterialValues(const renderer::ShaderVarDesc& variable,
                                        std::span<const double> values,
                                        std::span<uint8_t> destination);

/// @note t0-t4: 標準 PBR スロット。t5-t7: カスタムシェーダー用汎用スロット。
/// @note t8 = TEX_SHADOW はエンジン側で予約済みのため除外する。
inline constexpr std::array<const char*, 8> kMaterialTextureSlotNames = {
    "albedo",
    "normal",
    "metallic",
    "emissive",
    "ao",
    "tex5",
    "tex6",
    "tex7",
};

/// @note .mat の params から 1 変数ぶんを引く。手書き .mat 向けの snake_case 別名も見る。
[[nodiscard]] const std::vector<float>* FindMaterialParam(
    const MaterialAsset& asset, std::string_view shaderVarName);

/// @note シェーダーの全 float 変数へ既定値を敷く。.mat の適用より前に呼ぶこと。
void InitDefaultMaterialParams(const renderer::ShaderDescriptor& desc,
                               std::vector<uint8_t>& paramData);

/// @note .mat の params を名前で束縛する。
void ApplyMaterialAssetParams(const MaterialAsset& asset,
                              const renderer::ShaderDescriptor& desc,
                              std::vector<uint8_t>& paramData);

/// @note 共有アセット適用後に「この描画専用」の上書きを重ねる (per-instance パラメータ)。
void ApplyMaterialParamOverrides(
    const std::unordered_map<std::string, std::vector<float>>& overrides,
    const renderer::ShaderDescriptor& desc,
    std::vector<uint8_t>& paramData);

void ApplyMaterialIntegerOverrides(
    const std::unordered_map<std::string, std::vector<int64_t>>& overrides,
    const renderer::ShaderDescriptor& desc,
    std::vector<uint8_t>& paramData);

/// @note .mat の textures をスロット順の相対パス配列へ落とす。未設定スロットは空文字列。
[[nodiscard]] std::array<std::string, kMaterialTextureSlotNames.size()>
ResolveMaterialTexturePaths(const MaterialAsset& asset);

} // namespace fbzz::asset
