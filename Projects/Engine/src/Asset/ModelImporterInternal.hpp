// FBZZ Engine
// ModelImporterInternal.hpp | fbzz::asset
// ModelImporter を複数 Translation Unit に分割するための内部宣言。
// WHY: anonymous namespace では TU 間でシンボルを共有できないため、
//      fbzz::asset 名前空間で宣言し各 .cpp が include して利用する。
//      このヘッダーは src/Asset/ 専用であり、外部 include/ には置かない。
#pragma once
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

// ── Assimp 型変換 ──────────────────────────────────────────────
// aiString を std::string へ変換する。
std::string      ToString(const aiString& s);
// パス区切り文字を '/' に正規化した名前を返す。
std::string      NormalizeName(const aiString& s);
// unitScale: ReadUnitScale() で取得したメートル換算係数。
math::Vector3    ToVector3(const aiVector3D& v, float unitScale);
math::Quaternion ToQuaternion(const aiQuaternion& q);
// 平行移動成分にのみ unitScale を乗じる (回転・スケールは無次元量)。
math::Matrix4    ToMatrix4(const aiMatrix4x4& m, float unitScale);

// ── シーンメタデータ ───────────────────────────────────────────
// FBX の UnitScaleFactor メタデータからメートル換算係数を取得する (デフォルト 0.01)。
float ReadUnitScale(const aiScene* scene);
// アニメーションまたはボーンを持つ場合 true を返す。
bool  HasSkinning(const aiScene* scene);

// ── 共有メッシュ変換 ───────────────────────────────────────────
// 静的・スキンメッシュ両パスで使用する頂点 / インデックス / マテリアル変換。
renderer::Vertex              ImportVertex(const aiMesh* mesh, uint32_t i, float unitScale);
std::vector<uint32_t>         ImportIndices(const aiMesh* mesh);
std::shared_ptr<renderer::Material> ImportMaterial(const aiScene* scene,
                                                    const aiMesh* mesh,
                                                    renderer::ResourceManager& resources);

// ── サブインポーター ───────────────────────────────────────────
// ModelImporter::Import() から呼び出す。各 .cpp ファイルで定義する。
std::shared_ptr<Model> ImportStaticModel(const aiScene* scene,
                                         float unitScale,
                                         renderer::ResourceManager& resources);
std::shared_ptr<Model> ImportSkinnedModel(const aiScene* scene,
                                          float unitScale,
                                          renderer::ResourceManager& resources);

} // namespace fbzz::asset
