/// @file    ModelNode.hpp
/// @brief   DCC (FBX) のノード階層 — メッシュを GameObject へどう配るかの対応表。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// @note 新形式の ModelAsset (.fzasset) と旧 Model の両方がこの型を持つ必要があるため独立させた。
///       どちらか一方へ置くと他方が相手をインクルードし「新形式が旧形式に依存する」逆向きの
///       依存が生まれる。
#pragma once
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

/// @brief DCC のノード 1 個ぶん = Unity の 1 GameObject に対応する描画単位。
/// @note Assimp の scene->mMeshes は DCC メッシュをマテリアルごとに分割した結果を平坦に
///       並べたもの (2 マテリアルなら aiMesh 2 件)。それを「1 件 = 1 GameObject」にすると
///       DCC 上で 1 個だったオブジェクトが散らばり階層構造が読めなくなるため、Unity と同じく
///       ノードを分割単位にし、ノード内のマテリアル分割は Renderer の materials 配列の要素
///       として区別する。
struct ModelNode {
    std::string name;
    int parentIndex = -1;
    std::vector<int> children;

    /// 親ノードからの相対変換 (バインドポーズ)。生成する GameObject の Transform になる。
    math::Vector3    localTranslation = math::Vector3::ZERO;
    math::Quaternion localRotation    = math::Quaternion::Identity();
    math::Vector3    localScale       = math::Vector3::ONE;

    /// このノードが描くメッシュ配列の添字。Assimp のマテリアル分割により複数になる。
    /// 並び順がそのまま Renderer の submesh 順 = MaterialComponent のスロット順になる。
    /// 空のノードは Transform だけを持つグループ / ボーンで、Renderer は生成しない。
    std::vector<uint32_t> meshIndices;

    [[nodiscard]] bool HasMeshes() const { return !meshIndices.empty(); }
};

} // namespace fbzz::asset
