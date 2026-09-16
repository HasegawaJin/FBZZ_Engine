/// @file    ModelNode.hpp
/// @brief   DCC (FBX) のノード階層 — メッシュを GameObject へどう配るかの対応表。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY 独立したヘッダーにするか:
/// 新形式の ModelAsset (.fzasset) と旧 Model の両方がこの型を持つ必要がある。
/// どちらか一方へ置くと他方が相手をインクルードすることになり、
/// 「新形式が旧形式に依存する」という逆向きの依存が生まれる。
#pragma once
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

// DCC のノード 1 個ぶん = Unity の 1 GameObject に対応する描画単位。
//
// WHY メッシュ配列を平坦に並べるだけでは足りないか (重要):
//   Assimp の scene->mMeshes は「1 つの DCC メッシュをマテリアルごとに分割した」結果を
//   平坦に並べたものである。Body に 2 マテリアルが載っていれば aiMesh は 2 件になる。
//   この配列だけを見て「1 件 = 1 GameObject」にすると、DCC 上で 1 個だったオブジェクトが
//   マテリアル数ぶんの GameObject に散らばり、階層から元の構造が読めなくなる。
//   Unity が分割単位にしているのはノードで、ノード内のマテリアル分割は Renderer の
//   materials 配列の要素として扱われる。その区別を保つための対応表がこれ。
struct ModelNode {
    std::string name;
    int parentIndex = -1;
    std::vector<int> children;

    // 親ノードからの相対変換 (バインドポーズ)。生成する GameObject の Transform になる。
    math::Vector3    localTranslation = math::Vector3::ZERO;
    math::Quaternion localRotation    = math::Quaternion::Identity();
    math::Vector3    localScale       = math::Vector3::ONE;

    // このノードが描くメッシュ配列の添字。Assimp のマテリアル分割により複数になる。
    // 並び順がそのまま Renderer の submesh 順 = MaterialComponent のスロット順になる。
    // 空のノードは Transform だけを持つグループ / ボーンで、Renderer は生成しない。
    std::vector<uint32_t> meshIndices;

    [[nodiscard]] bool HasMeshes() const { return !meshIndices.empty(); }
};

} // namespace fbzz::asset
