/// @file MaterialParamBinding.hpp
/// @brief .mat の params / textures を、シェーダーリフレクションに従って生バイト列へ束縛する
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY 描画パスから切り出すか:
///   この束縛は元々 GeometryPassHelpers.cpp の無名名前空間にあり、メッシュ描画からしか
///   呼べなかった。UI のように「同じ .mat 形式を、違う頂点入力とパスで描く」側が
///   増えるたびに写経することになり、写した先だけが既定値やスロット名の変更に
///   追従しなくなる。束縛の規則は 1 箇所に置き、パスは結果を受け取るだけにする。
///
/// WHY ShaderDescriptor 経由か:
///   .mat は「シェーダー変数名 → float 列」で値を持ち、バイトオフセットを持たない。
///   オフセットはシェーダーをコンパイルして初めて決まるので、保存時に焼き付けると
///   シェーダーを 1 行足しただけで全 .mat が壊れる。名前で引き直す。
#pragma once

#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

/// t0-t4: 標準 PBR スロット。t5-t7: カスタムシェーダー用汎用スロット。
/// t8 = TEX_SHADOW はエンジン側で予約済みのため除外する。
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

/// .mat の params から 1 変数ぶんを引く。手書き .mat 向けの snake_case 別名も見る。
[[nodiscard]] const std::vector<float>* FindMaterialParam(
    const MaterialAsset& asset, std::string_view shaderVarName);

/// シェーダーの全 float 変数へ既定値を敷く。.mat の適用より前に呼ぶこと。
void InitDefaultMaterialParams(const renderer::ShaderDescriptor& desc,
                               std::vector<uint8_t>& paramData);

/// .mat の params を名前で束縛する。
void ApplyMaterialAssetParams(const MaterialAsset& asset,
                              const renderer::ShaderDescriptor& desc,
                              std::vector<uint8_t>& paramData);

/// 共有アセット適用後に「この描画専用」の上書きを重ねる (per-instance パラメータ)。
void ApplyMaterialParamOverrides(
    const std::unordered_map<std::string, std::vector<float>>& overrides,
    const renderer::ShaderDescriptor& desc,
    std::vector<uint8_t>& paramData);

/// .mat の textures をスロット順の相対パス配列へ落とす。未設定スロットは空文字列。
[[nodiscard]] std::array<std::string, kMaterialTextureSlotNames.size()>
ResolveMaterialTexturePaths(const MaterialAsset& asset);

} // namespace fbzz::asset
