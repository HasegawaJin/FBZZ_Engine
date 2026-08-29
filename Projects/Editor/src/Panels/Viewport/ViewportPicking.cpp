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

    // 正投影の視線はすべて平行で、カメラ位置を通らない。原点はカーソル位置の近平面上の点。
    // WHY 分けるか: Ray::FromNDC はカメラ位置を原点に固定するため、正投影では
    //     画面中央以外のクリックが常に中央付近を貫くレイになり、まるで当たらない。
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
    if (!ctx.activeScene || util::FileSystem::GetExtension(assetPath) != ".prefab")
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

    // .fbx は Unity と同じく直接ドロップ可能。未インポートならその場で自動インポートする。
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

    // .mat のドロップは UpdateMaterialDragPreview / CommitMaterialDragPreview 側で
    // ホバープレビュー付きで処理するため、ここでは扱わない。
    if (ext == ".mat") return false;

    if (ext != ".prefab") return false;
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
    float           bestFallbackDistSq = 1e30f; // カーソルからの画面距離^2 (主キー)
    scene::EntityID iconHit      = scene::EntityID::INVALID;
    float           iconBestDistSq = 1e30f;
    // 三角形の厳密ヒットが取れなかった細かい/薄いオブジェクト用の救済ヒット。
    // WHY: 遠くのフェンス支柱や小道具は画面上の投影が数ピクセルしかなく、
    //      ピクセル単位の三角形レイキャストではまず当たらない。バウンディング球を
    //      画面空間へ投影し、最低 kMinClickPx の当たり半径を保証することで
    //      Unity のコライダーピッキング相当の「多少それても拾ってくれる」体験にする。
    scene::EntityID nearMissHit       = scene::EntityID::INVALID;
    float           nearMissBestDistSq = 1e30f; // カーソルからの画面距離^2 (主キー)
    float           nearMissBestT      = 1e30f; // 同距離の場合のみ深度で決着

    auto transformPoint = [](const math::Matrix4& m, const math::Vector3& p) {
        math::Vector4 v = m * math::Vector4{ p.x, p.y, p.z, 1.0f };
        if (!math::NearlyZero(v.w)) v = v * (1.0f / v.w);
        return math::Vector3{ v.x, v.y, v.z };
    };

    // レイと球の交差 (原点が球内でもヒット扱い)。三角形総当たり前の事前カットに使う。
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

            if (t < bestT) {
                // ほぼ同距離 (親子で重なる/重複したジオメトリ) の場合、単純な t 比較だと
                // GameObjects() の走査順 (多くの場合、親が先に生成される) に選択が
                // 引っ張られてしまう。Unity と同じく、僅差の場合は子孫側を優先する。
                const float eps = (std::max)(bestT, t) * 1e-4f + 1e-5f;
                if (best.IsValid() && (bestT - t) < eps) {
                    auto* currentGo = ctx.activeScene->GetGameObject(best);
                    auto* newGo     = ctx.activeScene->GetGameObject(id);
                    if (currentGo && newGo && currentGo->IsDescendantOf(*newGo)) {
                        // 現在の best が新候補の子孫 = 既に子供が勝っている。維持する。
                        continue;
                    }
                }
                bestT = t;
                best  = id;
            } else if (best.IsValid() && best != id) {
                // best の方が僅かに近いが、新候補がその子孫なら子を優先して奪う。
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

    // メッシュのバウンディング球 (ローカル) をワールドへ変換して事前カットする。
    // WHY: 三角形総当たりは大規模シーンでクリックごとにヒッチを起こす。
    //      球テストで外れたメッシュの頂点走査を丸ごと省略する。
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

    // 三角形ヒットが得られなかった場合の救済判定。バウンディング球の中心・半径を
    // スクリーン空間へ投影し、最低 kMinClickPx px の当たり判定円をカーソルに与える。
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

        // 主キーは「カーソルにどれだけ近いか (画面距離)」。深度 (t) は同着に近い場合の
        // タイブレークにのみ使う。以前は t だけで比較していたため、複数の子オブジェクトが
        // 近接しているとカーソル位置に関係なく常にカメラへ最も近い1個 (=多くの場合、
        // 生成順が早く GameObjects() の先頭に近いオブジェクト) が固定的に選ばれてしまい、
        // 「一番上の子が強制選択される」ように見えていた。
        constexpr float kDistTieToleranceSq = 4.0f * 4.0f; // 4px 以内は同着とみなす
        const bool closerOnScreen = distSq < nearMissBestDistSq - kDistTieToleranceSq;
        const bool tiedOnScreen   = distSq < nearMissBestDistSq + kDistTieToleranceSq;
        if (closerOnScreen || (tiedOnScreen && t < nearMissBestT)) {
            nearMissBestDistSq = distSq;
            nearMissBestT      = t;
            nearMissHit        = id;
        }
    };

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;

        auto* mr  = go.GetComponent<scene::MeshRenderer>();
        auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>();
        auto* light  = go.GetComponent<scene::LightComponent>();
        auto* camera = go.GetComponent<scene::CameraComponent>();
        const math::Matrix4 world = go.transform.GetWorldMatrix();

        if (mr && mr->mesh) {
            testMeshWithBounds(*mr->mesh, mr->mesh->cpuVertices, world, go.transform, go.GetID(), 1.0f);
            considerNearMiss(mr->mesh->boundsCenter, mr->mesh->boundsRadius, world, go.transform, go.GetID());
        }

        if (smr && smr->model) {
            // どの submesh に当たっても選ばれるのはこの Renderer の GameObject 自身。
            // 描画されていない (非表示スロットの) submesh は判定から外す。
            // mi はローカルスロット番号なので、マテリアルスロットとそのまま対応する。
            const auto* mat = go.GetComponent<scene::MaterialComponent>();
            for (size_t mi = 0; mi < smr->SubmeshCount(); ++mi) {
                const renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
                if (!meshPtr) continue;
                if (mat && !mat->SlotAt(mi).visible) continue;
                // WHY: スキンメッシュはアニメーションでバインドポーズより外へ動くため、
                //      バウンディング球を 2 倍に膨らませて事前カットの取りこぼしを防ぐ。
                testMeshWithBounds(*meshPtr, meshPtr->cpuSkinnedVertices, world, go.transform, go.GetID(), 2.0f);
                considerNearMiss(meshPtr->boundsCenter, meshPtr->boundsRadius, world, go.transform, go.GetID());
            }
        }

        // ライト / カメラはスクリーン固定サイズのアイコンで描かれるため、
        // 3D レイではなくアイコン中心とのスクリーン距離でヒットテストする (Unity 互換)。
        if (light || camera) {
            ImVec2 sp;
            if (WorldToScreen(go.transform.position, ctx, viewportMin, vpSize, sp)) {
                constexpr float kIconRadius = 14.0f;
                const float dx = mouse.x - sp.x;
                const float dy = mouse.y - sp.y;
                const float distSq = dx * dx + dy * dy;
                if (distSq < kIconRadius * kIconRadius && distSq < iconBestDistSq) {
                    iconBestDistSq = distSq;
                    iconHit = go.GetID();
                }
            }
        }

        // メッシュもアイコンも無い空 GO だけ、原点近くの小球でフォールバック選択を許す。
        // WHY: 以前は全 GO を球判定していたため、メッシュの背後にある無関係な空 GO が
        //      クリックで選ばれてしまうことがあった。
        if (!mr && !smr && !light && !camera) {
            const math::Vector3 center = go.transform.position;
            const float radius = (std::max)(0.5f, go.transform.worldScale.Length() / 3.0f);
            float t = 0.0f;
            if (ray.IntersectSphere(center, radius, t) && t > 0.0f) {
                // ランク付けの主キーは「カーソルとの画面距離」。以前は t (カメラからの
                // 距離) だけで比較していたため、空 GO の子が密集している場面(スポーン地点、
                // アイテムスロット等)では、クリック位置に関係なく常にカメラへ最も近い1個
                // (=多くの場合、生成順が早いオブジェクト) が固定的に選ばれてしまっていた。
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
                    // 画面外 (カメラ背面等) への投影に失敗した場合は従来通り深度のみで比較する。
                    bestFallbackT = t;
                    bestFallback  = go.GetID();
                }
            }
        }
    }

    // Terrain: DDA レイキャストで正確に拾う (TerrainTool の実装を共用)。
    // ツールで編集中はブラシ操作を優先し、選択を変えない。
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

    // Water: 中心原点 ±extent/2 の水平面としてヒットテストする。
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

        // ローカル t をワールド距離へ換算して他の候補と比較する
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

    // 三角形の厳密ヒットが無ければ、画面空間の救済判定 (小さいオブジェクト) を採用する。
    if (!best.IsValid() && nearMissHit.IsValid()) {
        best = nearMissHit;
        bestT = nearMissBestT;
    }

    if (!best.IsValid() && bestFallback.IsValid()) {
        best = bestFallback;
        bestT = bestFallbackT;
    }

    // アイコンはスクリーン固定サイズで最前面に描かれるため、メッシュより優先する
    if (iconHit.IsValid())
        best = iconHit;

    return best;
}

namespace {

// マテリアルを受け取れるのはメッシュを描画するオブジェクトだけ。
// WHY: ピッキングはライト等のアイコンにもヒットするため、そのまま適用すると
//      見た目が何も変わらない MaterialComponent がライトに生えてしまう。
bool AcceptsMaterialDrop(scene::GameObject& go)
{
    return go.GetComponent<scene::MeshRenderer>() != nullptr
        || go.GetComponent<scene::SkinnedMeshRenderer>() != nullptr;
}

// .mat のパスを GameObject へ適用する (プレビュー / 確定 / Undo で共用)。
void AssignMaterialPath(scene::GameObject& go, const std::string& materialPath)
{
    auto* mc = go.GetComponent<scene::MaterialComponent>();
    if (!mc) mc = &go.AddComponent<scene::MaterialComponent>();
    mc->materialPath = materialPath;
    // ハンドルとキャッシュを無効化して次フレームの SyncMaterial に新パスを再解決させる。
    mc->materialAsset = {};
    mc->material.reset();
}

// 仮適用を巻き戻す。元々 MaterialComponent が無かった場合はコンポーネントごと取り除く。
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

    // 同じ対象へ同じ .mat を仮適用済みなら何もしない (毎フレームの再解決を避ける)。
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

    // 仮適用の状態をそのまま確定させる (見た目はドラッグ中から変わらない)。
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
        // Ctrl+クリック: 未選択なら追加、選択済みなら解除 (Unity 互換のトグル)
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

    // Ctrl なしは置き換え、Ctrl ありは追加 (Unity の矩形選択と同じ)
    std::vector<scene::EntityID> picked;
    if (ImGui::GetIO().KeyCtrl) picked = ctx.selectedEntities;

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        const scene::EntityID id = go.GetID();
        if (ctx.IsLocked(id)) continue;

        ImVec2 sp;
        if (!WorldToScreen(go.transform.position, ctx, vpMin, vpSize, sp)) continue;
        if (sp.x < rectMin.x || sp.x > rectMax.x || sp.y < rectMin.y || sp.y > rectMax.y) continue;

        if (std::find(picked.begin(), picked.end(), id) == picked.end())
            picked.push_back(id);
    }

    // 矩形選択は「今見えている範囲を囲った」操作なので、Hierarchy を先頭要素へ
    // 飛ばさない (数十件を選んだ直後にツリーが跳ねる方が邪魔になる)。
    SelectEntities(ctx, std::move(picked), SelectionReveal::Skip);
}

} // namespace fbzz::editor
