/// @file    ClothAsset.hpp
/// @brief   布の描画形状、明示的な質点対応と固定点を保持するアセット。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Renderer/Mesh.hpp>
#include <Math/Matrix4.hpp>
#include <array>
#include <span>
#include <string_view>

namespace fbzz::asset {
struct Skeleton;
struct ClothSkinBone {
    std::string name;
    math::Matrix4 inverseBind = math::Matrix4::Identity();
};
/// @note position はスキニング前の元頂点座標。ウェイトは非負・正規化済み、正ウェイトのボーンは重複しない。
struct ClothSkinWeight {
    math::Vector3 position{};
    std::array<uint32_t, 4> boneIndices{};
    std::array<float, 4> weights{};
};
/// @note ローカル座標 [m]。同位置でも別の質点 ID は溶接しない。三角形は描画頂点を参照する。
/// @see Docs/design/cloth.md
struct ClothAsset {
    std::vector<renderer::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<math::Vector3> particles;
    std::vector<uint32_t> renderToParticle;
    std::vector<uint32_t> pins;
    std::vector<ClothSkinBone> skinBones;
    std::vector<ClothSkinWeight> skinWeights;
    /// @note 読み込みごとに更新し、ホットリロード時にソルバーを再構築する。ファイルには保存しない。
    uint64_t revision = 0;
};

/// @return 不正な面、未使用質点、範囲外の対応・固定点、非有限値なら false。
[[nodiscard]] bool ValidateClothAsset(const ClothAsset& asset);
/// @note mapping が空なら 1:1。指定時は密な質点 ID を使用し、同じ ID の位置は一致させる。
/// @return 失敗時は out を変更しない。スキンメッシュには CreateSkinnedClothAsset を使用する。
[[nodiscard]] bool CreateClothAsset(const renderer::Mesh& mesh, std::span<const uint32_t> mapping, ClothAsset& out);
/// @note referencePose を静止形状に焼き、元の逆バインドと最大 4 ウェイトを質点へ転送する。seam のウェイト不一致は拒否する。
/// @return 失敗時は out を変更しない。Skeleton の referencePose は構築済みであること。
[[nodiscard]] bool CreateSkinnedClothAsset(const renderer::Mesh& mesh, const Skeleton& skeleton,
                                          std::span<const uint32_t> mapping, ClothAsset& out);
/// @note boneWorld は skinBones 順のワールド行列。out は失敗時に変更しない。
/// @see https://github.khronos.org/glTF-Tutorials/gltfTutorial/gltfTutorial_020_Skins.html Joint matrices と線形ブレンド。
[[nodiscard]] bool EvaluateClothSkinning(const ClothAsset& asset, std::span<const math::Matrix4> boneWorld,
                                        std::vector<math::Vector3>& out);
/// @return 読み込み・検証失敗時は false、out は未変更。
[[nodiscard]] bool LoadClothAssetFromFile(std::string_view path, ClothAsset& out);
/// @note 検証後、テンポラリから原子的に置き換える。
[[nodiscard]] bool SaveClothAssetToFile(std::string_view path, const ClothAsset& asset);
}
