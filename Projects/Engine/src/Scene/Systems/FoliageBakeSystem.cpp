// FBZZ Engine
// FoliageBakeSystem.cpp | fbzz::scene
// STAMP モードの FoliageSpecies 1スタンプにつき子 GO 階層を1つ生成・管理する。
// 生成される GO 構造:
//   TerrainGO
//     __<SpeciesName>_<idx>  ← stamp ルート: BoxColliderComponent (OBB)
// 子 GO は "__" プレフィクスにより SceneSerializer から除外される（非永続化）。
// WHY: SCATTER は GPU instancing で数千インスタンスを扱うため GO 化しない。
//      STAMP は個別配置で数が限られるため GO 化してヒエラルキー・物理を統合する。
//      レンダリングは引き続き FoliageRenderSystem (GPU instancing) が担う。
#include "Engine/Scene/Systems/FoliageBakeSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/FoliageComponent.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/Model.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/OBBCollider.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <string>

namespace fbzz::scene {

namespace {

// 親の world 値と子の local 値を合成して child の world 値を設定する。
// WHY: TransformSystem はフレーム先頭で実行済みのため、生成直後の子 GO は
//      worldPosition={0,0,0} のまま。同フレームの Debug Draw / Physics が
//      正しい位置を参照できるよう手動で設定する。
void SyncWorldFromParent(Transform& child, const Transform& parent)
{
    const math::Vector3 scaledLocal = {
        child.position.x * parent.worldScale.x,
        child.position.y * parent.worldScale.y,
        child.position.z * parent.worldScale.z,
    };
    const math::Vector3 rotated = parent.worldRotation * scaledLocal;
    child.worldPosition = {
        parent.worldPosition.x + rotated.x,
        parent.worldPosition.y + rotated.y,
        parent.worldPosition.z + rotated.z,
    };
    child.worldRotation = parent.worldRotation * child.rotation;
    child.worldScale    = {
        parent.worldScale.x * child.scale.x,
        parent.worldScale.y * child.scale.y,
        parent.worldScale.z * child.scale.z,
    };
}

// OBB コライダーの寸法 (すべて unscaled モデルローカル空間)。
// halfExtents: OBB の各軸半幅
// centerY:     GO 原点からの Y オフセット (モデルが Y=0 ベースでない場合に使用)
struct OBBDims {
    math::Vector3 halfExtents = { 0.5f, 2.0f, 0.5f };
    float         centerY     = 1.0f;
};

// モデルの CPU 頂点 AABB から OBB コライダー寸法を計算する。
// WHY: 手動設定ではスケールと形状が合わないことが多いため自動導出する。
OBBDims ComputeOBBFromModel(const std::string& modelPath)
{
    const auto* model = asset::AssetManager::Load<asset::Model>(modelPath);
    if (!model || model->meshes.empty())
        return {};

    math::Vector3 minV = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
    math::Vector3 maxV = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool hasData = false;

    for (const auto& mesh : model->meshes) {
        if (!mesh) continue;
        if (!mesh->cpuVertices.empty()) {
            for (const auto& v : mesh->cpuVertices) {
                minV.x = std::min(minV.x, v.position.x);
                minV.y = std::min(minV.y, v.position.y);
                minV.z = std::min(minV.z, v.position.z);
                maxV.x = std::max(maxV.x, v.position.x);
                maxV.y = std::max(maxV.y, v.position.y);
                maxV.z = std::max(maxV.z, v.position.z);
            }
            hasData = true;
        } else if (mesh->boundsRadius > 0.0f) {
            // cpuVertices なし → バウンディング球で AABB を近似
            const float r = mesh->boundsRadius;
            const math::Vector3& c = mesh->boundsCenter;
            minV.x = std::min(minV.x, c.x - r); maxV.x = std::max(maxV.x, c.x + r);
            minV.y = std::min(minV.y, c.y - r); maxV.y = std::max(maxV.y, c.y + r);
            minV.z = std::min(minV.z, c.z - r); maxV.z = std::max(maxV.z, c.z + r);
            hasData = true;
        }
    }

    if (!hasData) return {};

    const math::Vector3 halfExtents = {
        std::max(0.05f, (maxV.x - minV.x) * 0.5f),
        std::max(0.05f, (maxV.y - minV.y) * 0.5f),
        std::max(0.05f, (maxV.z - minV.z) * 0.5f),
    };
    return { halfExtents, minV.y + halfExtents.y };
}

} // namespace

ComponentAccess FoliageBakeSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<TerrainComponent, FoliageComponent>()
        .Writes<FoliageComponent>();
}

void FoliageBakeSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    for (EntityID eid : scene.GetEntities<FoliageComponent>()) {
        auto* foliage  = scene.GetComponent<FoliageComponent>(eid);
        auto* go       = scene.GetGameObject(eid);
        if (!foliage || !go || !foliage->enabled || !foliage->needsBakeChildren)
            continue;

        // Stamp 子 GO は TerrainComponent を持つ GO の子にのみ生成する
        if (!scene.GetComponent<TerrainComponent>(eid))
            continue;

        foliage->needsBakeChildren = false;

        // 旧子 GO を破棄 (DestroyGameObject は子孫を再帰削除する)
        for (auto& child : foliage->childEntities)
            scene.DestroyGameObject(child.entityID);
        foliage->childEntities.clear();

        for (size_t si = 0; si < foliage->species.size(); ++si) {
            const auto& species = foliage->species[si];
            if (species.placementMode != FoliagePlacementMode::STAMP) continue;
            if (species.modelPath.empty()) continue;
            if (species.stamps.empty()) continue;

            // "Oak" のように種名として使うため basename を抽出
            std::string speciesName = species.modelPath;
            if (const auto slash = speciesName.find_last_of("/\\");
                slash != std::string::npos)
                speciesName = speciesName.substr(slash + 1);
            if (const auto dot = speciesName.rfind('.');
                dot != std::string::npos)
                speciesName = speciesName.substr(0, dot);

            // OBB 寸法を決定 (スタンプ全体で共有、unscaled モデルローカル空間)
            // manual=true のとき保存値をそのまま使う。false のとき AABB から自動計算する。
            // WHY: 木はキャノピー幅 != trunk 幅のため AABB 自動計算が trunk と合わないことがある。
            OBBDims dims{};
            if (species.colliderEnabled) {
                if (species.colliderManual) {
                    const float hw = std::max(0.01f, species.colliderHalfWidth);
                    const float hh = std::max(0.01f, species.colliderHalfHeight);
                    dims.halfExtents = { hw, hh, hw };
                    dims.centerY     = hh;
                } else {
                    dims = ComputeOBBFromModel(species.modelPath);
                }
            }

            for (size_t stampIdx = 0; stampIdx < species.stamps.size(); ++stampIdx) {
                const auto& stamp = species.stamps[stampIdx];
                const float s = std::max(0.01f, stamp.scale);

                const math::Quaternion stampRot =
                    math::Quaternion::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, stamp.rotationY);

                // ── stamp ルート GO ──────────────────────────────────────────
                const std::string rootName =
                    "__" + speciesName + "_" + std::to_string(stampIdx);
                auto& root = scene.CreateGameObject(rootName);

                // 親子関係: TerrainGO の子にする
                root.SetParent(*go);

                // ローカル座標 = stamp の terrain ローカル位置
                root.transform.position = stamp.localPosition;
                root.transform.rotation = stampRot;
                root.transform.scale    = { s, s, s };

                // 同フレームで world 値を手動同期
                SyncWorldFromParent(root.transform, go->transform);

                // OBB コライダー
                if (species.colliderEnabled) {
                    // size は full extents (PhysicsSystem の SyncColliderShape が size*0.5 を halfExtents に渡す)
                    // スタンプスケール s を事前乗算する。OBBCollider は worldScale を自動適用しないため。
                    BoxColliderComponent box;
                    box.size = {
                        dims.halfExtents.x * 2.0f * s,
                        dims.halfExtents.y * 2.0f * s,
                        dims.halfExtents.z * 2.0f * s,
                    };
                    // center は unscaled ローカル空間 (ColliderWorldCenter が worldScale を掛ける)
                    box.center  = { 0.0f, dims.centerY, 0.0f };
                    box.collider = std::make_unique<physics::OBBCollider>(box.size * 0.5f);
                    root.AddComponent<BoxColliderComponent>(std::move(box));
                }

                foliage->childEntities.push_back({ root.GetID(), species.colliderCullDistance });
            }
        }

        // 子 GO が生成された場合、ヒエラルキーパネルへ親ノード展開を要求する。
        if (!foliage->childEntities.empty())
            foliage->needsHierarchyExpand = true;
    }
}

} // namespace fbzz::scene
