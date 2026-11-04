/// @file    ModelImporterInternal.hpp
/// @brief   ModelImporter を複数 Translation Unit に分割するための内部宣言。
/// @author  Hasegawa Jin
/// @date    2026-05-28

/// @note anonymous namespace では TU 間でシンボルを共有できないため、fbzz::asset 名前空間で
/// @note 宣言し各 .cpp が include して利用する。このヘッダーは src/Asset/ 専用であり、
/// @note 外部 include/ には置かない。
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/Matrix4.hpp>
#include <assimp/scene.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::asset {

/// @note aiString を std::string へ変換する。
FBZZ_ENGINE_API std::string      ToString(const aiString& s);
/// @note パス区切り文字を '/' に正規化した名前を返す。
FBZZ_ENGINE_API std::string      NormalizeName(const aiString& s);
/// @note unitScale: ReadUnitScale() で取得したメートル換算係数。
FBZZ_ENGINE_API math::Vector3    ToVector3(const aiVector3D& v, float unitScale);
FBZZ_ENGINE_API math::Quaternion ToQuaternion(const aiQuaternion& q);
/// @note 平行移動成分にのみ unitScale を乗じる (回転・スケールは無次元量)。
FBZZ_ENGINE_API math::Matrix4    ToMatrix4(const aiMatrix4x4& m, float unitScale);

/// @note FBX の UnitScaleFactor メタデータからメートル換算係数を取得する (デフォルト 0.01)。
FBZZ_ENGINE_API float ReadUnitScale(const aiScene* scene);
/// @note アニメーションまたはボーンを持つ場合 true を返す。
FBZZ_ENGINE_API bool  HasSkinning(const aiScene* scene);

/// @note 静的・スキンメッシュ両パスで使用する頂点 / インデックス / マテリアル変換。
FBZZ_ENGINE_API renderer::Vertex              ImportVertex(const aiMesh* mesh, uint32_t i, float unitScale);
FBZZ_ENGINE_API std::vector<uint32_t>         ImportIndices(const aiMesh* mesh);
FBZZ_ENGINE_API std::unique_ptr<renderer::Material> ImportMaterial(const aiScene* scene,
                                                    const aiMesh* mesh,
                                                    renderer::ResourceManager& resources);

/// @brief aiNode を再帰的に辿り Model::nodes / rootNodeIndex を構築する。
/// @note aiNode::mMeshes をそのまま ModelNode::meshIndices へ写すため、DCC 上で 1 個だった
/// @note オブジェクトがマテリアル分割で複数 aiMesh になっていても 1 ノードに束ねられる。
/// @note aiProcess_PreTransformVertices を通したシーンでは階層が既に潰れているため、
/// @note 呼んでも「全メッシュを持つルート 1 個」しか得られない。
FBZZ_ENGINE_API void ImportModelNodes(const aiScene* scene, float unitScale, Model& model);

/// @brief 階層情報を持たないシーン用に「全メッシュを担当するルート 1 個」を作る。
/// @note 呼び出し側 (spawner) に「nodes が空なら別の経路」という分岐を書かせないため、
/// @note 平坦なシーンも 1 ノードのツリーとして同じ形で表現しておく。
FBZZ_ENGINE_API void BuildFlatModelNode(Model& model, const std::string& rootName);

/// @note ModelImporter::Import() から呼び出す。各 .cpp ファイルで定義する。
FBZZ_ENGINE_API std::unique_ptr<Model> ImportStaticModel(const aiScene* scene,
                                         float unitScale,
                                         renderer::ResourceManager& resources);
FBZZ_ENGINE_API std::unique_ptr<Model> ImportSkinnedModel(const aiScene* scene,
                                          float unitScale,
                                          renderer::ResourceManager& resources);

} /// @note namespace fbzz::asset
