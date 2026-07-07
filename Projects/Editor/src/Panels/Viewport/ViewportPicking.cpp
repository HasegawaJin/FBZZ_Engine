// FBZZ Engine
// ViewportPicking.cpp | fbzz::editor
// Scene View のアセットドロップと3Dピッキング
#include "ViewportCommon.hpp"
#include <Editor/Util/ModelPlacement.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Util/StringUtils.hpp>

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

    ctx.selectedEntities = roots;
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
        ctx.selectedEntities = { root };
        return true;
    }

    // .mat はドロップ位置のオブジェクトへマテリアルを適用する (Unity と同じ操作感)。
    if (ext == ".mat") {
        if (!PickEntity(ctx, viewportMin) || ctx.selectedEntities.empty()) return false;
        auto* go = ctx.activeScene->GetGameObject(ctx.selectedEntities.front());
        if (!go) return false;
        auto* mc = go->GetComponent<scene::MaterialComponent>();
        if (!mc) mc = &go->AddComponent<scene::MaterialComponent>();
        mc->materialPath = assetPath;
        // ハンドルを無効化して次フレームの SyncMaterial に新パスを再解決させる。
        mc->materialAsset = {};
        return true;
    }

    if (ext != ".prefab") return false;
    if (!InstantiatePrefabAsset(ctx, assetPath)) return false;
    for (scene::EntityID id : ctx.selectedEntities) {
        if (auto* go = ctx.activeScene->GetGameObject(id))
            go->transform.position = position;
    }
    return true;
}

void HandleGizmoShortcuts(EditorContext& ctx)
{
    // WHY: エンジン生入力 (input::Input) は ImGui のキーボードキャプチャを知らない。
    //      ゲートなしだと Inspector で名前入力中の W/E/R でもギズモモードが切り替わり、
    //      カメラフライ (右ドラッグ + WASD) 中の W とも衝突する。
    //      Unity と同じく「ビューポートをホバー中 or フォーカス中」かつ
    //      「テキスト入力中でない」「右ドラッグ中でない」ときだけ受け付ける。
    if (ImGui::GetIO().WantTextInput) return;
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
        !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;

    if (input::Input::KeyDown(input::KeyCode::W)) ctx.gizmoMode = EditorContext::GizmoMode::Translate;
    if (input::Input::KeyDown(input::KeyCode::E)) ctx.gizmoMode = EditorContext::GizmoMode::Rotate;
    if (input::Input::KeyDown(input::KeyCode::R)) ctx.gizmoMode = EditorContext::GizmoMode::Scale;
    if (input::Input::KeyDown(input::KeyCode::Q)) {
        ctx.gizmoSpace = (ctx.gizmoSpace == EditorContext::GizmoSpace::World)
            ? EditorContext::GizmoSpace::Local
            : EditorContext::GizmoSpace::World;
    }
}

bool PickEntity(EditorContext& ctx, const ImVec2& viewportMin)
{
    if (!ctx.activeScene || !ctx.editorCamera) return false;

    const math::Ray ray = ScreenRayFromMouse(ctx, viewportMin);
    const ImVec2 vpSize = { ctx.viewportWidth, ctx.viewportHeight };
    const ImVec2 mouse  = ImGui::GetMousePos();

    scene::EntityID best         = scene::EntityID::INVALID;
    float           bestT        = 1e30f;
    scene::EntityID bestFallback = scene::EntityID::INVALID;
    float           bestFallbackT = 1e30f;
    scene::EntityID iconHit      = scene::EntityID::INVALID;
    float           iconBestDistSq = 1e30f;

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
            if (ray.IntersectTriangle(v0, v1, v2, t) && t < bestT) {
                bestT = t;
                best  = id;
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

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;

        auto* mr  = go.GetComponent<scene::MeshRenderer>();
        auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>();
        auto* light  = go.GetComponent<scene::LightComponent>();
        auto* camera = go.GetComponent<scene::CameraComponent>();
        const math::Matrix4 world = go.transform.GetWorldMatrix();

        if (mr && mr->mesh)
            testMeshWithBounds(*mr->mesh, mr->mesh->cpuVertices, world, go.transform, go.GetID(), 1.0f);

        if (smr && smr->model) {
            for (const auto& meshPtr : smr->model->meshes) {
                if (!meshPtr) continue;
                // WHY: スキンメッシュはアニメーションでバインドポーズより外へ動くため、
                //      バウンディング球を 2 倍に膨らませて事前カットの取りこぼしを防ぐ。
                testMeshWithBounds(*meshPtr, meshPtr->cpuSkinnedVertices, world, go.transform, go.GetID(), 2.0f);
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
            if (ray.IntersectSphere(center, radius, t) && t < bestFallbackT) {
                bestFallbackT = t;
                bestFallback  = go.GetID();
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

    // アイコンはスクリーン固定サイズで最前面に描かれるため、メッシュより優先する
    if (iconHit.IsValid())
        best = iconHit;

    if (!best.IsValid() && bestFallback.IsValid()) {
        best = bestFallback;
        bestT = bestFallbackT;
    }

    if (best.IsValid() && !ctx.IsLocked(best)) {
        auto& sel = ctx.selectedEntities;
        const auto it = std::find(sel.begin(), sel.end(), best);
        if (ImGui::GetIO().KeyCtrl) {
            // Ctrl+クリック: 未選択なら追加、選択済みなら解除 (Unity 互換のトグル)
            if (it != sel.end())
                sel.erase(it);
            else
                sel.push_back(best);
        } else {
            sel.clear();
            sel.push_back(best);
        }
        return true;
    }

    if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
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
    if (!ImGui::GetIO().KeyCtrl)
        ctx.selectedEntities.clear();

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        const scene::EntityID id = go.GetID();
        if (ctx.IsLocked(id)) continue;

        ImVec2 sp;
        if (!WorldToScreen(go.transform.position, ctx, vpMin, vpSize, sp)) continue;
        if (sp.x < rectMin.x || sp.x > rectMax.x || sp.y < rectMin.y || sp.y > rectMax.y) continue;

        if (std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id)
            == ctx.selectedEntities.end())
            ctx.selectedEntities.push_back(id);
    }
}

} // namespace fbzz::editor
