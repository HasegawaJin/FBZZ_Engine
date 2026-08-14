// FBZZ Engine
// ViewportSnapping.cpp | fbzz::editor
// 頂点スナップ (V ドラッグ) と面スナップ (Ctrl+Shift ドラッグ)
//
// WHY: 座標グリッドスナップだけでは「地形の起伏に建物を接地させる」「隣の壁と隙間なく
//      並べる」ができず、目視 + 数値打ちに頼ることになる。どちらも配置作業では毎回出るので、
//      ImGuizmo を介さない独立のドラッグ操作として実装する。
//
//      ImGuizmo に混ぜないのは、これらが「ギズモの軸に沿った移動」ではなく
//      「掴んだ点をカーソル下の点へ吸着させる」操作で、軸ハンドルの概念と噛み合わないため。
//      スナップ中は呼び出し側 (ViewportPanel) がギズモ・ピッキング・矩形選択を止める。
#include "ViewportCommon.hpp"
#include <Editor/Util/UndoStack.hpp>

namespace fbzz::editor {

namespace {

// ドラッグ 1 回ぶんの状態。
struct SnapDrag {
    bool active     = false;
    bool vertexMode = false;   // true=頂点スナップ / false=面スナップ
    // 掴んだ点とプライマリ原点のワールドオフセット。
    // WHY: 頂点スナップは「掴んだ頂点」を目標へ合わせる操作なので、
    //      オブジェクトの原点ではなくこのオフセットぶんずらして配置する。
    math::Vector3 grabOffset{};
    math::Vector3 grabWorld{};  // 表示用 (掴んでいる点)
    bool          hasTarget = false;
    math::Vector3 targetWorld{};

    std::vector<std::string>      guids;   // 移動対象 (top-level 選択のみ)
    std::vector<scene::Transform> before;  // Undo 用
};
SnapDrag g_drag;

math::Vector3 TransformPoint(const math::Matrix4& m, const math::Vector3& p)
{
    math::Vector4 v = m * math::Vector4{ p.x, p.y, p.z, 1.0f };
    if (!math::NearlyZero(v.w)) v = v * (1.0f / v.w);
    return { v.x, v.y, v.z };
}

bool IsSelected(const EditorContext& ctx, scene::EntityID id)
{
    return std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id)
        != ctx.selectedEntities.end();
}

// 選択済みの祖先を持たないか (親子同時選択で二重移動しないための判定)。
bool HasSelectedAncestor(const EditorContext& ctx, scene::GameObject* obj)
{
    for (scene::GameObject* p = obj->GetParent(); p; p = p->GetParent())
        if (IsSelected(ctx, p->GetID())) return true;
    return false;
}

// レイがメッシュのワールドバウンディング球に当たるか。
// WHY: 頂点走査もサーフェス交差も、素で回すと 1 フレームあたり数十万頂点になる。
//      カーソルのレイと交わらないメッシュは中身を一切見ずに捨てる (PickEntity と同じ手)。
bool RayHitsMeshBounds(const math::Ray& ray,
                       const renderer::Mesh& mesh,
                       const math::Matrix4& world,
                       const scene::Transform& tf,
                       float radiusInflate)
{
    if (mesh.boundsRadius <= 0.0f) return true;   // バウンズ未設定なら判定できないので通す

    const math::Vector3 center = TransformPoint(world, mesh.boundsCenter);
    const math::Vector3& ws = tf.worldScale;
    const float maxScale =
        (std::max)((std::max)(std::abs(ws.x), std::abs(ws.y)), std::abs(ws.z));
    const float radius = mesh.boundsRadius * (std::max)(maxScale, 0.0001f) * radiusInflate;

    const math::Vector3 toCenter = center - ray.origin;
    if (toCenter.Length() <= radius) return true;
    float t = 0.0f;
    return ray.IntersectSphere(center, radius, t);
}

// GameObject が持つメッシュの CPU 頂点を、ワールド座標で 1 つずつコールバックへ渡す。
// WHY: 頂点スナップは「選択側の頂点を探す」「非選択側の頂点を探す」の両方で同じ走査が要る。
// cursorRay に当たらないメッシュはバウンズ判定で丸ごと省く。
template<typename Fn>
void ForEachWorldVertex(scene::GameObject& go, const math::Ray& cursorRay, Fn&& fn)
{
    const math::Matrix4 world = go.transform.GetWorldMatrix();

    // WHY: 頂点はカーソルから数十 px 以内にあれば拾いたいので、球を少し膨らませて
    //      「カーソルがメッシュの縁を外れている」ケースを取りこぼさないようにする。
    constexpr float kInflate = 1.25f;

    if (auto* mr = go.GetComponent<scene::MeshRenderer>(); mr && mr->mesh) {
        if (RayHitsMeshBounds(cursorRay, *mr->mesh, world, go.transform, kInflate))
            for (const auto& v : mr->mesh->cpuVertices)
                fn(TransformPoint(world, v.position));
    }
    if (auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>(); smr && smr->model) {
        // 1 GameObject = モデル全体。描画していない (非表示スロットの) submesh へ
        // 吸着しないよう、可視スロットの頂点だけを対象にする。
        const auto* mat = go.GetComponent<scene::MaterialComponent>();
        for (size_t i = 0; i < smr->model->meshes.size(); ++i) {
            const auto& meshPtr = smr->model->meshes[i];
            if (!meshPtr) continue;
            if (mat && !mat->SlotAt(i).visible) continue;
            // スキンメッシュはバインドポーズより外へ動くため球を大きめに取る。
            if (!RayHitsMeshBounds(cursorRay, *meshPtr, world, go.transform, kInflate * 2.0f))
                continue;
            for (const auto& v : meshPtr->cpuSkinnedVertices)
                fn(TransformPoint(world, v.position));
        }
    }
}

// カーソルに最も近い頂点をスクリーン空間で探す。
// selectedSide == true なら選択中のオブジェクトから、false なら非選択から探す。
bool FindNearestVertex(EditorContext& ctx,
                       const ImVec2& vpMin,
                       const ImVec2& vpSize,
                       const ImVec2& cursor,
                       bool selectedSide,
                       float maxPixelDist,
                       math::Vector3& outWorld)
{
    if (!ctx.activeScene) return false;

    float bestDistSq = maxPixelDist * maxPixelDist;
    bool  found      = false;

    const math::Ray cursorRay = ScreenRayFromMouse(ctx, vpMin);

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (IsSelected(ctx, go.GetID()) != selectedSide) continue;

        ForEachWorldVertex(go, cursorRay, [&](const math::Vector3& worldPos) {
            ImVec2 sp;
            if (!WorldToScreen(worldPos, ctx, vpMin, vpSize, sp)) return;
            const float dx = sp.x - cursor.x;
            const float dy = sp.y - cursor.y;
            const float distSq = dx * dx + dy * dy;
            if (distSq >= bestDistSq) return;
            bestDistSq = distSq;
            outWorld   = worldPos;
            found      = true;
        });
    }
    return found;
}

// UP から normal への最小回転を作る。
// WHY: Quaternion に FromToRotation が無いため、外積を軸・内積の acos を角度として組む。
//      ほぼ同方向 / ほぼ真逆は外積が退化するので個別に扱う。
math::Quaternion AlignUpToNormal(const math::Vector3& normal)
{
    const math::Vector3 up = math::Vector3::UP;
    const math::Vector3 n  = normal.Normalized();
    const float d = math::Vector3::Dot(up, n);
    if (d >  0.9999f) return math::Quaternion::Identity();
    if (d < -0.9999f) return math::Quaternion::FromAxisAngle({ 1.0f, 0.0f, 0.0f }, 3.14159265f);
    const math::Vector3 axis = math::Vector3::Cross(up, n).Normalized();
    return math::Quaternion::FromAxisAngle(axis, std::acos(d));
}

// 非選択オブジェクトのサーフェスへレイを飛ばし、ヒット点と法線を返す。
bool RaycastUnselectedSurface(EditorContext& ctx,
                              const math::Ray& ray,
                              const ImVec2& vpMin,
                              const ImVec2& vpSize,
                              math::Vector3& outPoint,
                              math::Vector3& outNormal)
{
    if (!ctx.activeScene) return false;

    float bestT  = 1e30f;
    bool  found  = false;

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (IsSelected(ctx, go.GetID())) continue;   // 自分自身へは接地しない

        const math::Matrix4 world = go.transform.GetWorldMatrix();

        auto testMesh = [&](const renderer::Mesh& mesh, const auto& verts, const auto& indices,
                            float radiusInflate) {
            if (verts.empty() || indices.size() < 3) return;
            // バウンズで外れたメッシュは三角形総当たりへ進まない。
            if (!RayHitsMeshBounds(ray, mesh, world, go.transform, radiusInflate)) return;
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                const std::uint32_t i0 = indices[i + 0];
                const std::uint32_t i1 = indices[i + 1];
                const std::uint32_t i2 = indices[i + 2];
                if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;

                const math::Vector3 v0 = TransformPoint(world, verts[i0].position);
                const math::Vector3 v1 = TransformPoint(world, verts[i1].position);
                const math::Vector3 v2 = TransformPoint(world, verts[i2].position);

                float t = 0.0f;
                if (!ray.IntersectTriangle(v0, v1, v2, t) || t >= bestT) continue;

                bestT     = t;
                outPoint  = ray.origin + ray.direction * t;
                outNormal = math::Vector3::Cross(v1 - v0, v2 - v0).Normalized();
                found     = true;
            }
        };

        if (auto* mr = go.GetComponent<scene::MeshRenderer>(); mr && mr->mesh)
            testMesh(*mr->mesh, mr->mesh->cpuVertices, mr->mesh->cpuIndices, 1.0f);
        if (auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>(); smr && smr->model) {
            // 1 GameObject = モデル全体。描画されている submesh だけを判定対象にする。
            const auto* mat = go.GetComponent<scene::MaterialComponent>();
            for (size_t i = 0; i < smr->model->meshes.size(); ++i) {
                const auto& meshPtr = smr->model->meshes[i];
                if (!meshPtr) continue;
                if (mat && !mat->SlotAt(i).visible) continue;
                testMesh(*meshPtr, meshPtr->cpuSkinnedVertices, meshPtr->cpuIndices, 2.0f);
            }
        }
    }

    // Terrain は専用の DDA レイキャストで拾う (メッシュ走査より速く、かつ正確)。
    if (ctx.terrainTool) {
        math::Vector3      hitWorld{};
        scene::GameObject* hitGO = nullptr;
        if (ctx.terrainTool->RaycastTerrain(*ctx.activeScene, *ctx.editorCamera,
                                            vpMin, vpSize,
                                            hitWorld, hitGO) && hitGO && !IsSelected(ctx, hitGO->GetID())) {
            const float t = math::Vector3::Dot(hitWorld - ray.origin, ray.direction);
            if (t > 0.0f && t < bestT) {
                bestT     = t;
                outPoint  = hitWorld;
                outNormal = math::Vector3::UP;   // 地形法線は取れないので上向き扱い
                found     = true;
            }
        }
    }

    return found;
}

// 移動対象 (選択済みの祖先を持たない選択) を集め、Undo 用の before も記録する。
void CollectDragTargets(EditorContext& ctx)
{
    g_drag.guids.clear();
    g_drag.before.clear();
    for (scene::EntityID id : ctx.selectedEntities) {
        scene::GameObject* go = ctx.activeScene->GetGameObject(id);
        if (!go || ctx.IsLocked(id)) continue;
        if (HasSelectedAncestor(ctx, go)) continue;
        g_drag.guids.push_back(go->instanceId);
        g_drag.before.push_back(go->transform);
    }
}

void PushSnapUndo(EditorContext& ctx, const char* description)
{
    if (!ctx.undoStack || !ctx.undoStack->IsRecordingEnabled() || !ctx.activeScene) return;
    if (g_drag.guids.empty()) return;

    scene::Scene* scene = ctx.activeScene;
    const std::vector<std::string>      guids  = g_drag.guids;
    const std::vector<scene::Transform> before = g_drag.before;

    std::vector<scene::Transform> after;
    after.reserve(guids.size());
    bool anyChanged = false;
    for (std::size_t i = 0; i < guids.size(); ++i) {
        auto* target = scene->FindByGuid(guids[i]);
        after.push_back(target ? target->transform : before[i]);
        if (target) {
            const auto& a = after.back();
            const auto& b = before[i];
            if (a.position.x != b.position.x || a.position.y != b.position.y ||
                a.position.z != b.position.z || a.rotation.x != b.rotation.x ||
                a.rotation.y != b.rotation.y || a.rotation.z != b.rotation.z ||
                a.rotation.w != b.rotation.w)
                anyChanged = true;
        }
    }
    if (!anyChanged) return;

    const auto markDirty = ctx.markSceneDirty;
    auto applyAll = [scene, guids, markDirty](const std::vector<scene::Transform>& values) {
        for (std::size_t i = 0; i < guids.size(); ++i)
            if (auto* target = scene->FindByGuid(guids[i]))
                target->transform = values[i];
        if (markDirty) markDirty();
    };
    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        description,
        [applyAll, after]()  { applyAll(after); },
        [applyAll, before]() { applyAll(before); }));
    if (markDirty) markDirty();
}

// 全対象を delta ぶん平行移動する。
void TranslateTargets(EditorContext& ctx, const math::Vector3& delta)
{
    if (math::NearlyZero(delta.x) && math::NearlyZero(delta.y) && math::NearlyZero(delta.z))
        return;
    for (const std::string& guid : g_drag.guids)
        if (auto* target = ctx.activeScene->FindByGuid(guid))
            target->transform.position = target->transform.position + delta;
}

// スナップ中のフィードバック描画 (掴んだ点と吸着先)。
void DrawSnapFeedback(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2      sp;

    if (WorldToScreen(g_drag.grabWorld, ctx, vpMin, vpSize, sp)) {
        dl->AddCircleFilled(sp, 5.0f, IM_COL32(255, 220, 60, 255));
        dl->AddCircle(sp, 8.0f, IM_COL32(0, 0, 0, 160), 12, 1.5f);
    }
    if (g_drag.hasTarget && WorldToScreen(g_drag.targetWorld, ctx, vpMin, vpSize, sp)) {
        dl->AddCircle(sp, 9.0f, IM_COL32(80, 230, 120, 255), 16, 2.5f);
        dl->AddLine({ sp.x - 12.0f, sp.y }, { sp.x + 12.0f, sp.y }, IM_COL32(80, 230, 120, 200), 1.5f);
        dl->AddLine({ sp.x, sp.y - 12.0f }, { sp.x, sp.y + 12.0f }, IM_COL32(80, 230, 120, 200), 1.5f);
    }
}

} // namespace

bool HandleViewportSnapping(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize)
{
    if (!ctx.activeScene || !ctx.editorCamera) { ctx.vertexSnapActive = ctx.surfaceSnapActive = false; return false; }

    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const bool hasSelection = !ctx.selectedEntities.empty();

    // 修飾キーの状態。V は「押している間」有効 (Unity と同じモーメンタリ操作)。
    const bool vertexKey  = !io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_V) && !io.KeyCtrl;
    const bool surfaceKey = io.KeyCtrl && io.KeyShift;

    ctx.vertexSnapActive  = hasSelection && hovered && vertexKey;
    ctx.surfaceSnapActive = hasSelection && hovered && surfaceKey;

    // ── ドラッグ開始 ─────────────────────────────────────────────────────────
    if (!g_drag.active &&
        (ctx.vertexSnapActive || ctx.surfaceSnapActive) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        scene::GameObject* primary = ctx.activeScene->GetGameObject(ctx.PrimarySelected());
        if (!primary) return false;

        CollectDragTargets(ctx);
        if (g_drag.guids.empty()) return false;

        g_drag.vertexMode = ctx.vertexSnapActive;
        g_drag.hasTarget  = false;

        // 頂点スナップは「カーソルに一番近い自分の頂点」を掴む。
        // 見つからなければ原点を掴んだ扱いにして、面スナップと同じ挙動へ倒す。
        math::Vector3 grab = primary->transform.position;
        if (g_drag.vertexMode) {
            constexpr float kGrabRadiusPx = 40.0f;
            math::Vector3   found{};
            if (FindNearestVertex(ctx, vpMin, vpSize, ImGui::GetMousePos(), true, kGrabRadiusPx, found))
                grab = found;
        }
        g_drag.grabWorld  = grab;
        g_drag.grabOffset = grab - primary->transform.position;
        g_drag.active     = true;
    }

    if (!g_drag.active) return false;

    // ── ドラッグ終了 ─────────────────────────────────────────────────────────
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        PushSnapUndo(ctx, g_drag.vertexMode ? "Vertex Snap" : "Surface Snap");
        g_drag.active = false;
        return true;   // このフレームはまだ他の操作へ渡さない
    }

    scene::GameObject* primary = ctx.activeScene->GetGameObject(ctx.PrimarySelected());
    if (!primary) { g_drag.active = false; return false; }

    // ── ドラッグ中: 吸着先を決めて全対象を平行移動 ──────────────────────────
    const ImVec2 cursor = ImGui::GetMousePos();
    g_drag.hasTarget = false;

    if (g_drag.vertexMode) {
        constexpr float kTargetRadiusPx = 60.0f;
        math::Vector3   target{};
        if (FindNearestVertex(ctx, vpMin, vpSize, cursor, false, kTargetRadiusPx, target)) {
            g_drag.targetWorld = target;
            g_drag.hasTarget   = true;
        }
    } else {
        const math::Ray ray = ScreenRayFromMouse(ctx, vpMin);
        math::Vector3   point{};
        math::Vector3   normal{};
        if (RaycastUnselectedSurface(ctx, ray, vpMin, vpSize, point, normal)) {
            g_drag.targetWorld = point;
            g_drag.hasTarget   = true;

            // 接地面の法線へ上方向を合わせる (任意)。位置より先に回してから移動する。
            if (ctx.surfaceSnapAlignToNormal) {
                const math::Quaternion align = AlignUpToNormal(normal);
                for (const std::string& guid : g_drag.guids)
                    if (auto* target = ctx.activeScene->FindByGuid(guid))
                        target->transform.rotation = align;
            }
        }
    }

    if (g_drag.hasTarget) {
        // 掴んだ点が目標へ来るように全対象を動かす。
        const math::Vector3 desiredPrimaryPos = g_drag.targetWorld - g_drag.grabOffset;
        const math::Vector3 delta = desiredPrimaryPos - primary->transform.position;
        TranslateTargets(ctx, delta);
        g_drag.grabWorld = g_drag.targetWorld;
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    DrawSnapFeedback(ctx, vpMin, vpSize);
    return true;
}

} // namespace fbzz::editor
