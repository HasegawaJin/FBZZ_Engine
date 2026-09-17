/// @file    ViewportPicking.cpp
/// @brief   Scene View のアセットドロップと3Dピッキング。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "ViewportCommon.hpp"
#include <Editor/Util/ModelPlacement.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <cmath>
#include <memory>

namespace fbzz::editor {

math::Matrix4 ToColumnMajor(const math::Matrix4& rowMajor)
{
    return math::Matrix4::Transpose(rowMajor);
}

math::Ray ScreenRayFromMouse(const EditorContext& ctx, const ImVec2& viewportMin)
{
    ImVec2 mouse = ImGui::GetMousePos();
    float nx = ((mouse.x - viewportMin.x) / ctx.viewportWidth)  * 2.0f - 1.0f;
    float ny = 1.0f - ((mouse.y - viewportMin.y) / ctx.viewportHeight) * 2.0f;

    const math::Matrix4 invVP = math::Matrix4::Inverse(
        ctx.editorCamera->GetProjectionMatrix() * ctx.editorCamera->GetViewMatrix());

    /// @note 正投影の視線は平行でカメラ位置を通らない。Ray::FromNDC は原点をカメラに固定するので、原点をカーソル下の近平面上へ置き直す。
    if (ctx.editorCamera->m_projection == renderer::ProjectionMode::Orthographic) {
        math::Vector4 nearWorld = invVP * math::Vector4{ nx, ny, 0.0f, 1.0f };
        if (std::fabs(nearWorld.w) > 1e-6f) nearWorld = nearWorld * (1.0f / nearWorld.w);
        return { { nearWorld.x, nearWorld.y, nearWorld.z }, ctx.editorCamera->GetForward() };
    }
    return math::Ray::FromNDC(nx, ny, ctx.editorCamera->m_position, invVP);
}

bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath)
{
    if (!payload || payload->DataSize <= 0) return false;
    outPath.assign(static_cast<const char*>(payload->Data),
                   static_cast<size_t>(payload->DataSize - 1));
    return !outPath.empty();
}

math::Vector3 PrefabDropPosition(const EditorContext& ctx, const ImVec2& viewportMin)
{
    const math::Ray ray = ScreenRayFromMouse(ctx, viewportMin);
    float planeT = 0.0f;
    if (ray.IntersectPlane(math::Plane({ 0.0f, 1.0f, 0.0f }, 0.0f), planeT) && planeT > 0.0f)
        return ray.At(planeT);
    return ray.At(5.0f);
}

bool InstantiatePrefabAsset(EditorContext& ctx, const std::string& assetPath)
{
    if (!ctx.activeScene) return false;
    if (!IsInstantiableAssetExtension(
            util::StringUtils::ToLower(util::FileSystem::GetExtension(assetPath))))
        return false;

    std::vector<scene::EntityID> roots;
    if (!PrefabSerializer::Instantiate(*ctx.activeScene, assetPath, roots))
        return false;

    SelectEntities(ctx, roots);
    return true;
}

bool InstantiateAssetAtViewport(EditorContext& ctx,
                                const std::string& assetPath,
                                const ImVec2& viewportMin)
{
    if (!ctx.activeScene) return false;
    const math::Vector3 position = PrefabDropPosition(ctx, viewportMin);
    const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(assetPath));

    /// @note .fbx は直接ドロップ可能。未インポートならその場で自動インポートする。
    std::string modelPath;
    if (ext == ".fbx") {
        modelPath = ResolveOrImportFbxModel(assetPath);
        if (modelPath.empty()) return false;
    }
    if (!modelPath.empty()) {
        const scene::EntityID root = SpawnModelAssetHierarchy(ctx, modelPath, &position);
        if (root == scene::EntityID::INVALID) return false;
        SelectEntity(ctx, root);
        return true;
    }

    /// @note .mat は UpdateMaterialDragPreview / CommitMaterialDragPreview がホバープレビュー付きで扱う。
    if (ext == ".mat") return false;

    if (!IsInstantiableAssetExtension(ext)) return false;
    if (!InstantiatePrefabAsset(ctx, assetPath)) return false;
    for (scene::EntityID id : ctx.selectedEntities) {
        if (auto* go = ctx.activeScene->GetGameObject(id))
            go->transform.position = position;
    }
    return true;
}

scene::EntityID RaycastEntityAtMouse(EditorContext& ctx, const ImVec2& viewportMin)
{
    if (!ctx.activeScene || !ctx.editorCamera) return scene::EntityID::INVALID;

    const math::Ray ray = ScreenRayFromMouse(ctx, viewportMin);
    const ImVec2 vpSize = { ctx.viewportWidth, ctx.viewportHeight };
    const ImVec2 mouse  = ImGui::GetMousePos();

    scene::EntityID best         = scene::EntityID::INVALID;
    float           bestT        = 1e30f;
    scene::EntityID bestFallback = scene::EntityID::INVALID;
    float           bestFallbackT = 1e30f;
    float           bestFallbackDistSq = 1e30f;
    scene::EntityID iconHit      = scene::EntityID::INVALID;
    float           iconBestDistSq = 1e30f;
    /// @note 三角形ヒットが取れない細い/遠い物の救済。バウンディング球を画面へ投影し、最低 kMinClickPx の当たり半径を保証する。
    /// @note 救済・フォールバックの順位は «カーソルとの画面距離» が主キー、深度 t は同着時だけ。t 主キーだと密集した子の先頭が固定で選ばれていた。
    scene::EntityID nearMissHit       = scene::EntityID::INVALID;
    float           nearMissBestDistSq = 1e30f;
    float           nearMissBestT      = 1e30f;

    auto transformPoint = [](const math::Matrix4& m, const math::Vector3& p) {
        math::Vector4 v = m * math::Vector4{ p.x, p.y, p.z, 1.0f };
        if (!math::NearlyZero(v.w)) v = v * (1.0f / v.w);
        return math::Vector3{ v.x, v.y, v.z };
    };

    /// @note レイと球の交差 (原点が球内でもヒット)。三角形総当たり前の事前カット。
    auto sphereHit = [&ray](const math::Vector3& c, float r) {
        const math::Vector3 toC = { c.x - ray.origin.x, c.y - ray.origin.y, c.z - ray.origin.z };
        if (toC.Length() <= r) return true;
        float t = 0.0f;
        return ray.IntersectSphere(c, r, t);
    };

    auto testMesh = [&](const auto& verts,
                        const auto& indices,
                        const math::Matrix4& world,
                        scene::EntityID id) {
        if (verts.empty() || indices.size() < 3) return;

        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const uint32_t i0 = indices[i + 0];
            const uint32_t i1 = indices[i + 1];
            const uint32_t i2 = indices[i + 2];
            if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;

            const math::Vector3 v0 = transformPoint(world, verts[i0].position);
            const math::Vector3 v1 = transformPoint(world, verts[i1].position);
            const math::Vector3 v2 = transformPoint(world, verts[i2].position);

            float t = 0.0f;
            if (!ray.IntersectTriangle(v0, v1, v2, t)) continue;

            /// @note 僅差 (親子で重なるジオメトリ) は走査順に引っ張られないよう、Unity と同じく子孫側を優先する。
            if (t < bestT) {
                const float eps = (std::max)(bestT, t) * 1e-4f + 1e-5f;
                if (best.IsValid() && (bestT - t) < eps) {
                    auto* currentGo = ctx.activeScene->GetGameObject(best);
                    auto* newGo     = ctx.activeScene->GetGameObject(id);
                    if (currentGo && newGo && currentGo->IsDescendantOf(*newGo)) {
                        continue;
                    }
                }
                bestT = t;
                best  = id;
            } else if (best.IsValid() && best != id) {
                const float eps = (std::max)(bestT, t) * 1e-4f + 1e-5f;
                if ((t - bestT) < eps) {
                    auto* currentGo = ctx.activeScene->GetGameObject(best);
                    auto* newGo     = ctx.activeScene->GetGameObject(id);
                    if (currentGo && newGo && newGo->IsDescendantOf(*currentGo)) {
                        bestT = t;
                        best  = id;
                    }
                }
            }
        }
    };

    /// @note 三角形総当たりは大規模シーンでクリックごとにヒッチする。球で外れたメッシュの頂点走査を丸ごと省く。
    auto testMeshWithBounds = [&](const renderer::Mesh& mesh,
                                  const auto& verts,
                                  const math::Matrix4& world,
                                  const scene::Transform& tf,
                                  scene::EntityID id,
                                  float radiusInflate) {
        if (mesh.boundsRadius > 0.0f) {
            const math::Vector3 centerWorld = transformPoint(world, mesh.boundsCenter);
            const math::Vector3& ws = tf.worldScale;
            const float maxScale = (std::max)((std::max)(std::abs(ws.x), std::abs(ws.y)), std::abs(ws.z));
            const float radiusWorld = mesh.boundsRadius * (std::max)(maxScale, 0.0001f) * radiusInflate;
            if (!sphereHit(centerWorld, radiusWorld)) return;
        }
        testMesh(verts, mesh.cpuIndices, world, id);
    };

    auto considerNearMiss = [&](const math::Vector3& centerLocal, float radiusLocal,
                                const math::Matrix4& world, const scene::Transform& tf,
                                scene::EntityID id) {
        if (radiusLocal <= 0.0f || !ctx.editorCamera) return;
        const math::Vector3& ws = tf.worldScale;
        const float maxScale = (std::max)((std::max)(std::abs(ws.x), std::abs(ws.y)), std::abs(ws.z));
        const math::Vector3 centerWorld = transformPoint(world, centerLocal);
        const float radiusWorld = radiusLocal * (std::max)(maxScale, 0.0001f);

        ImVec2 centerSp;
        if (!WorldToScreen(centerWorld, ctx, viewportMin, vpSize, centerSp)) return;
        const math::Vector3 edgeWorld = centerWorld + ctx.editorCamera->GetRight() * radiusWorld;
        ImVec2 edgeSp;
        if (!WorldToScreen(edgeWorld, ctx, viewportMin, vpSize, edgeSp)) return;

        constexpr float kMinClickPx = 6.0f;
        const float dxr = edgeSp.x - centerSp.x;
        const float dyr = edgeSp.y - centerSp.y;
        const float screenRadius = (std::max)(std::sqrt(dxr * dxr + dyr * dyr), kMinClickPx);

        const float dx = mouse.x - centerSp.x;
        const float dy = mouse.y - centerSp.y;
        const float distSq = dx * dx + dy * dy;
        if (distSq > screenRadius * screenRadius) return;

        const math::Vector3 toCenter = { centerWorld.x - ray.origin.x,
                                          centerWorld.y - ray.origin.y,
                                          centerWorld.z - ray.origin.z };
        const float t = math::Vector3::Dot(toCenter, ray.direction);
        if (t <= 0.0f) return;

        constexpr float kDistTieToleranceSq = 4.0f * 4.0f;
        const bool closerOnScreen = distSq < nearMissBestDistSq - kDistTieToleranceSq;
        const bool tiedOnScreen   = distSq < nearMissBestDistSq + kDistTieToleranceSq;
        if (closerOnScreen || (tiedOnScreen && t < nearMissBestT)) {
            nearMissBestDistSq = distSq;
            nearMissBestT      = t;
            nearMissHit        = id;
        }
    };

    /// @note アイコンはスクリーン固定サイズなので 3D レイでなくアイコン中心との画面距離で判定する。描画と同じ表・同じ投影結果を使う。
    std::vector<SceneIconInstance> icons;
    CollectSceneIcons(ctx, viewportMin, vpSize, icons);
    std::vector<uint64_t> iconOwners;
    iconOwners.reserve(icons.size());
    for (const SceneIconInstance& icon : icons) {
        iconOwners.push_back((static_cast<uint64_t>(icon.id.index) << 32) | icon.id.generation);
        const float hitRadius = SceneIconHitRadius(icon);
        const float dx = mouse.x - icon.screenPos.x;
        const float dy = mouse.y - icon.screenPos.y;
        const float distSq = dx * dx + dy * dy;
        if (distSq < hitRadius * hitRadius && distSq < iconBestDistSq) {
            iconBestDistSq = distSq;
            iconHit = icon.id;
        }
    }
    std::sort(iconOwners.begin(), iconOwners.end());

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;

        auto* mr  = go.GetComponent<scene::MeshRenderer>();
        auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>();
        const scene::EntityID goId = go.GetID();
        const bool hasIcon = std::binary_search(iconOwners.begin(), iconOwners.end(),
            (static_cast<uint64_t>(goId.index) << 32) | goId.generation);
        const math::Matrix4 world = go.transform.GetWorldMatrix();

        if (mr && mr->mesh) {
            testMeshWithBounds(*mr->mesh, mr->mesh->cpuVertices, world, go.transform, go.GetID(), 1.0f);
            considerNearMiss(mr->mesh->boundsCenter, mr->mesh->boundsRadius, world, go.transform, go.GetID());
        }

        if (smr && smr->model) {
            /// @note 非表示スロットの submesh は判定から外す。mi はローカルスロット番号でマテリアルスロットと一致する。
            /// @note スキンは動きでバインドポーズの外へ出るので、事前カットの球を 2 倍に膨らませる。
            const auto* mat = go.GetComponent<scene::MaterialComponent>();
            for (size_t mi = 0; mi < smr->SubmeshCount(); ++mi) {
                const renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
                if (!meshPtr) continue;
                if (mat && !mat->SlotAt(mi).visible) continue;
                testMeshWithBounds(*meshPtr, meshPtr->cpuSkinnedVertices, world, go.transform, go.GetID(), 2.0f);
                considerNearMiss(meshPtr->boundsCenter, meshPtr->boundsRadius, world, go.transform, go.GetID());
            }
        }

        /// @note メッシュもアイコンも無い GO だけ原点の小球でフォールバック選択を許す。全 GO を球判定するとメッシュの背後の空 GO が選ばれる。
        if (!mr && !smr && !hasIcon) {
            const math::Vector3 center = go.transform.worldPosition;
            const float radius = (std::max)(0.5f, go.transform.worldScale.Length() / 3.0f);
            float t = 0.0f;
            if (ray.IntersectSphere(center, radius, t) && t > 0.0f) {
                ImVec2 sp;
                if (WorldToScreen(center, ctx, viewportMin, vpSize, sp)) {
                    const float dx = mouse.x - sp.x;
                    const float dy = mouse.y - sp.y;
                    const float distSq = dx * dx + dy * dy;
                    constexpr float kDistTieToleranceSq = 4.0f * 4.0f;
                    const bool closerOnScreen = distSq < bestFallbackDistSq - kDistTieToleranceSq;
                    const bool tiedOnScreen   = distSq < bestFallbackDistSq + kDistTieToleranceSq;
                    if (closerOnScreen || (tiedOnScreen && t < bestFallbackT)) {
                        bestFallbackDistSq = distSq;
                        bestFallbackT      = t;
                        bestFallback       = go.GetID();
                    }
                } else if (t < bestFallbackT) {
                    /// @note カメラ背面などで投影できなければ深度だけで比べる。
                    bestFallbackT = t;
                    bestFallback  = go.GetID();
                }
            }
        }
    }

    /// @note Terrain は TerrainTool の DDA レイキャストを共用する。ツール編集中はブラシを優先して選択を変えない。
    if (ctx.terrainTool && !ctx.terrainTool->IsActive()) {
        math::Vector3 hitWorld{};
        scene::GameObject* hitGO = nullptr;
        if (ctx.terrainTool->RaycastTerrain(*ctx.activeScene, *ctx.editorCamera,
                                            viewportMin, vpSize, hitWorld, hitGO) && hitGO) {
            const math::Vector3 toHit = {
                hitWorld.x - ray.origin.x, hitWorld.y - ray.origin.y, hitWorld.z - ray.origin.z };
            const float t = math::Vector3::Dot(toHit, ray.direction);
            if (t > 0.0f && t < bestT) {
                bestT = t;
                best  = hitGO->GetID();
            }
        }
    }

    /// @note Water は中心原点 ±extent/2 の水平面として判定し、ローカル t をワールド距離へ換算して比べる。
    for (scene::EntityID eid : ctx.activeScene->GetEntities<scene::WaterComponent>()) {
        auto* water = ctx.activeScene->GetComponent<scene::WaterComponent>(eid);
        auto* go    = ctx.activeScene->GetGameObject(eid);
        if (!water || !go || !water->enabled || !go->activeInHierarchy()) continue;

        const math::Matrix4 invWorld = math::Matrix4::Inverse(go->transform.GetWorldMatrix());
        const math::Vector4 lo = invWorld * math::Vector4{ ray.origin.x, ray.origin.y, ray.origin.z, 1.0f };
        const math::Vector4 ld = invWorld * math::Vector4{ ray.direction.x, ray.direction.y, ray.direction.z, 0.0f };
        if (std::abs(ld.y) < 1e-6f) continue;
        const float tLocal = -lo.y / ld.y;
        if (tLocal <= 0.0f) continue;
        const float lx = lo.x + ld.x * tLocal;
        const float lz = lo.z + ld.z * tLocal;
        if (std::abs(lx) > water->extentX * 0.5f || std::abs(lz) > water->extentZ * 0.5f) continue;

        const math::Vector3 hitLocal = { lx, 0.0f, lz };
        const math::Vector3 hitWorld = transformPoint(go->transform.GetWorldMatrix(), hitLocal);
        const math::Vector3 toHit = {
            hitWorld.x - ray.origin.x, hitWorld.y - ray.origin.y, hitWorld.z - ray.origin.z };
        const float t = math::Vector3::Dot(toHit, ray.direction);
        if (t > 0.0f && t < bestT) {
            bestT = t;
            best  = go->GetID();
        }
    }

    if (!best.IsValid() && nearMissHit.IsValid()) {
        best = nearMissHit;
        bestT = nearMissBestT;
    }

    if (!best.IsValid() && bestFallback.IsValid()) {
        best = bestFallback;
        bestT = bestFallbackT;
    }

    /// @note アイコンは最前面に描かれるのでメッシュより優先する。
    if (iconHit.IsValid())
        best = iconHit;

    return best;
}

namespace {

/// @return メッシュを描くオブジェクトだけ true。
/// @note ピッキングはアイコンにも当たるので、弾かないと見た目の変わらない MaterialComponent がライト等に生える。
bool AcceptsMaterialDrop(scene::GameObject& go)
{
    return go.GetComponent<scene::MeshRenderer>() != nullptr
        || go.GetComponent<scene::SkinnedMeshRenderer>() != nullptr;
}

/// @brief .mat のパスを適用する (プレビュー / 確定 / Undo で共用)。
/// @note ハンドルとキャッシュを捨て、次フレームの SyncMaterial に新パスを解決させる。
void AssignMaterialPath(scene::GameObject& go, const std::string& materialPath)
{
    auto* mc = go.GetComponent<scene::MaterialComponent>();
    if (!mc) mc = &go.AddComponent<scene::MaterialComponent>();
    mc->materialPath = materialPath;
    mc->materialAsset = {};
    mc->material.reset();
}

/// @brief 仮適用を巻き戻す。元々 MaterialComponent が無ければコンポーネントごと取り除く。
void RevertMaterialPreview(EditorContext& ctx, MaterialDragPreviewState& state)
{
    if (state.applied && ctx.activeScene) {
        if (auto* go = ctx.activeScene->GetGameObject(state.target)) {
            if (state.hadComponent) {
                if (auto* mc = go->GetComponent<scene::MaterialComponent>()) {
                    mc->materialPath  = state.previousPath;
                    mc->materialAsset = {};
                    mc->material.reset();
                }
            } else {
                go->RemoveComponent<scene::MaterialComponent>();
            }
        }
    }
    state.applied      = false;
    state.hadComponent = false;
    state.target       = scene::EntityID::INVALID;
    state.previousPath.clear();
}

} // namespace

void UpdateMaterialDragPreview(EditorContext& ctx,
                               MaterialDragPreviewState& state,
                               const std::string& materialPath,
                               const ImVec2& viewportMin)
{
    if (!ctx.activeScene || materialPath.empty()) {
        CancelMaterialDragPreview(ctx, state);
        return;
    }

    scene::EntityID hovered = RaycastEntityAtMouse(ctx, viewportMin);
    if (hovered.IsValid() && ctx.IsLocked(hovered))
        hovered = scene::EntityID::INVALID;
    if (hovered.IsValid()) {
        auto* go = ctx.activeScene->GetGameObject(hovered);
        if (!go || !AcceptsMaterialDrop(*go))
            hovered = scene::EntityID::INVALID;
    }

    if (state.applied && state.target == hovered && state.materialPath == materialPath)
        return;

    RevertMaterialPreview(ctx, state);
    state.materialPath = materialPath;
    if (!hovered.IsValid())
        return;

    auto* go = ctx.activeScene->GetGameObject(hovered);
    if (!go) return;

    const auto* mc     = go->GetComponent<scene::MaterialComponent>();
    state.hadComponent = mc != nullptr;
    state.previousPath = mc ? mc->materialPath : std::string{};
    state.target       = hovered;
    state.applied      = true;
    AssignMaterialPath(*go, materialPath);
}

void CancelMaterialDragPreview(EditorContext& ctx, MaterialDragPreviewState& state)
{
    RevertMaterialPreview(ctx, state);
    state.materialPath.clear();
}

bool CommitMaterialDragPreview(EditorContext& ctx, MaterialDragPreviewState& state)
{
    if (!state.applied || !ctx.activeScene) {
        CancelMaterialDragPreview(ctx, state);
        return false;
    }

    const scene::EntityID target      = state.target;
    const std::string     newPath     = state.materialPath;
    const std::string     oldPath     = state.previousPath;
    const bool            hadComponent = state.hadComponent;

    state.applied = false;
    state.target  = scene::EntityID::INVALID;
    state.previousPath.clear();
    state.hadComponent = false;
    state.materialPath.clear();

    if (ctx.undoStack) {
        EditorContext* context = &ctx;
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Assign Material",
            [context, target, newPath]() {
                if (!context->activeScene) return;
                if (auto* go = context->activeScene->GetGameObject(target))
                    AssignMaterialPath(*go, newPath);
            },
            [context, target, oldPath, hadComponent]() {
                if (!context->activeScene) return;
                auto* go = context->activeScene->GetGameObject(target);
                if (!go) return;
                if (hadComponent) {
                    AssignMaterialPath(*go, oldPath);
                } else {
                    go->RemoveComponent<scene::MaterialComponent>();
                }
            }));
    }
    return true;
}

bool PickEntity(EditorContext& ctx, const ImVec2& viewportMin)
{
    const scene::EntityID best = RaycastEntityAtMouse(ctx, viewportMin);

    if (best.IsValid() && !ctx.IsLocked(best)) {
        if (ImGui::GetIO().KeyCtrl) ToggleSelection(ctx, best);
        else                        SelectEntity(ctx, best);
        return true;
    }

    if (!ImGui::GetIO().KeyCtrl) ClearEntitySelection(ctx);
    return false;
}

void RectSelectEntities(EditorContext& ctx,
                        const ImVec2& vpMin,
                        const ImVec2& vpSize,
                        const ImVec2& rectA,
                        const ImVec2& rectB)
{
    if (!ctx.activeScene || !ctx.editorCamera) return;

    const ImVec2 rectMin = { (std::min)(rectA.x, rectB.x), (std::min)(rectA.y, rectB.y) };
    const ImVec2 rectMax = { (std::max)(rectA.x, rectB.x), (std::max)(rectA.y, rectB.y) };

    std::vector<scene::EntityID> picked;
    if (ImGui::GetIO().KeyCtrl) picked = ctx.selectedEntities;

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        const scene::EntityID id = go.GetID();
        if (ctx.IsLocked(id)) continue;

        ImVec2 sp;
        if (!WorldToScreen(go.transform.worldPosition, ctx, vpMin, vpSize, sp)) continue;
        if (sp.x < rectMin.x || sp.x > rectMax.x || sp.y < rectMin.y || sp.y > rectMax.y) continue;

        if (std::find(picked.begin(), picked.end(), id) == picked.end())
            picked.push_back(id);
    }

    /// @note 矩形選択では Hierarchy を先頭要素へ飛ばさない。数十件を選んだ直後にツリーが跳ねる方が邪魔になる。
    SelectEntities(ctx, std::move(picked), SelectionReveal::Skip);
}

} // namespace fbzz::editor
