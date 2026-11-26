/// @file    ClothAsset.hpp
/// @brief   布の描画形状、明示的な質点対応と固定点を保持するアセット。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Api.hpp>
#include <Graphics/Renderer/Geometry/Mesh.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <array>
#include <span>
#include <string_view>

namespace fbzz::asset {
struct Skeleton;
struct ClothSkinBone {
    std::string name; ///< 変形先の骨格ノードを照合するスキニング骨の名前。
    math::Matrix4 inverseBind = math::Matrix4::Identity(); ///< バインド姿勢で頂点を対応する骨空間へ変換する逆バインド行列。
};
/// @note position はスキニング前の元頂点座標。ウェイトは非負・正規化済み、正ウェイトのボーンは重複しない。
struct ClothSkinWeight {
    math::Vector3 position{}; ///< スキニングで変形を加える、バインド時の布の位置。
    std::array<uint32_t, 4> boneIndices{}; ///< この位置に影響を与える skinBones の添字。最大 4 骨。
    std::array<float, 4> weights{}; ///< boneIndices と同じ並びで保持する骨の影響重み。
};
/// @note 三角形の重心座標と、辺方向・面内直交方向・法線方向のローカル [m] オフセット。
struct ClothRenderBinding {
    std::array<uint32_t, 3> particles{}; ///< 描画頂点を復元する三角形のソルバー粒子の添字。
    math::Vector3 barycentric{1, 0, 0}; ///< particles の 3 点を補間する重心座標。
    math::Vector3 offset{}; ///< 粒子三角形から補間した位置に追加する描画頂点のオフセット。
};
/// @note ローカル座標 [m]。同位置でも別の質点 ID は溶接しない。三角形は描画頂点を参照する。
/// @see Docs/design/cloth.md
struct ClothAsset {
    std::vector<renderer::Vertex> vertices; ///< 布の描画メッシュの元頂点列。
    std::vector<uint32_t> indices; ///< vertices を参照する描画用の三角形インデックス列。
    std::vector<math::Vector3> particles; ///< 布ソルバーへ渡す初期の粒子位置列。
    std::vector<uint32_t> renderToParticle; ///< 描画頂点の添字から対応するソルバー粒子の添字を引く表。
    std::vector<uint32_t> simulationIndices; ///< renderBindings 使用時は物理三角形を独立して保存し、renderToParticle は空にする。
    std::vector<ClothRenderBinding> renderBindings; ///< 描画頂点をソルバー粒子三角形の変形へ対応付ける表。
    std::vector<uint32_t> pins; ///< ソルバーで固定する初期粒子の添字列。
    std::vector<ClothSkinBone> skinBones; ///< 布のスキニング入力に使う骨と逆バインドの列。
    std::vector<ClothSkinWeight> skinWeights; ///< スキニングする位置ごとの骨番号と重み列。
    uint64_t revision = 0; ///< 読み込みごとに更新し、ホットリロード時にソルバーを再構築する。ファイルには保存しない。
};

/// @return 不正な面、未使用質点、範囲外の対応・固定点、非有限値なら false。
[[nodiscard]] FBZZ_ENGINE_API bool ValidateClothAsset(const ClothAsset& asset);
/// @note 両メッシュは同じモデル空間。最近接三角形まで maxDistance [m] 以内のみ結合する。失敗時 out は未変更。
/// @see https://realtimecollisiondetection.net/ Real-Time Collision Detection 5.1.5 Closest Point on Triangle。
[[nodiscard]] FBZZ_ENGINE_API bool BindClothRenderMesh(const ClothAsset& simulation, const renderer::Mesh& renderMesh,
                                      float maxDistance, ClothAsset& out);
/// @note positions と rest は同じローカル空間。潰れた三角形のオフセット方向は静止形状を使う。失敗時 out は未変更。
[[nodiscard]] FBZZ_ENGINE_API bool EvaluateClothRenderBindings(std::span<const ClothRenderBinding> bindings,
    std::span<const math::Vector3> rest, std::span<const math::Vector3> positions, std::vector<math::Vector3>& out);
/// @note mapping が空なら 1:1。指定時は密な質点 ID を使用し、同じ ID の位置は一致させる。
/// @return 失敗時は out を変更しない。スキンメッシュには CreateSkinnedClothAsset を使用する。
[[nodiscard]] FBZZ_ENGINE_API bool CreateClothAsset(const renderer::Mesh& mesh, std::span<const uint32_t> mapping, ClothAsset& out);
/// @note referencePose を静止形状に焼き、元の逆バインドと最大 4 ウェイトを質点へ転送する。seam のウェイト不一致は拒否する。
/// @return 失敗時は out を変更しない。Skeleton の referencePose は構築済みであること。
[[nodiscard]] FBZZ_ENGINE_API bool CreateSkinnedClothAsset(const renderer::Mesh& mesh, const Skeleton& skeleton,
                                          std::span<const uint32_t> mapping, ClothAsset& out);
/// @note boneWorld は skinBones 順のワールド行列。out は失敗時に変更しない。
/// @see https://github.khronos.org/glTF-Tutorials/gltfTutorial/gltfTutorial_020_Skins.html Joint matrices と線形ブレンド。
[[nodiscard]] FBZZ_ENGINE_API bool EvaluateClothSkinning(const ClothAsset& asset, std::span<const math::Matrix4> boneWorld,
                                        std::vector<math::Vector3>& out);
/// @return 読み込み・検証失敗時は false、out は未変更。
[[nodiscard]] FBZZ_ENGINE_API bool LoadClothAssetFromFile(std::string_view path, ClothAsset& out);
/// @note 検証後、テンポラリから原子的に置き換える。
[[nodiscard]] FBZZ_ENGINE_API bool SaveClothAssetToFile(std::string_view path, const ClothAsset& asset);
}
