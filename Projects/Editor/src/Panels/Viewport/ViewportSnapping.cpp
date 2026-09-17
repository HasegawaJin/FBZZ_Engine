/// @file    ViewportSnapping.cpp
/// @brief   頂点スナップ (V ドラッグ) と面スナップ (Ctrl+Shift ドラッグ)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note ImGuizmo に混ぜないのは、軸に沿った移動ではなく «掴んだ点をカーソル下の点へ吸着させる» 操作だから。
/// @note スナップ中は呼び出し側 (ViewportPanel) がギズモ・ピッキング・矩形選択を止める。
#include "ViewportCommon.hpp"
#include <Editor/Util/UndoStack.hpp>

namespace fbzz::editor {

namespace {

/// @brief ドラッグ 1 回ぶんの状態。位置はすべてワールド空間。
struct SnapDrag {
    bool active     = false;
    bool vertexMode = false;   ///< true=頂点スナップ / false=面スナップ
    /// @note 頂点スナップは掴んだ頂点を目標へ合わせるので、原点ではなくこのオフセットぶんずらして置く。
    math::Vector3 grabOffset{};
    math::Vector3 grabWorld{};
    bool          hasTarget = false;
    math::Vector3 targetWorld{};

    std::vector<std::string>      guids;   ///< 移動対象 (選択済みの祖先を持たないものだけ)
    std::vector<scene::Transform> before;  ///< Undo 用
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

/// @return 選択済みの祖先を持つなら true (親子同時選択で二重移動させない)。
bool HasSelectedAncestor(const EditorContext& ctx, scene::GameObject* obj)
{
    for (scene::GameObject* p = obj->GetParent(); p; p = p->GetParent())
        if (IsSelected(ctx, p->GetID())) return true;
    return false;
}

/// @brief ローカル position から今のワールド位置を組み直す。
/// @note worldPosition は TransformSystem が次フレームに更新するので、ドラッグ中に書き換えた直後は古い。親は動かない (祖先は対象外) ので親の行列は信用できる。
math::Vector3 CurrentWorldPosition(const scene::GameObject& go)
{
    if (const scene::GameObject* parent = go.GetParent())
        return TransformPoint(parent->transform.GetWorldMatrix(), go.transform.position);
    return go.transform.position;
}

/// @return レイがメッシュのワールドバウンディング球に当たるなら true。バウンズ未設定なら判定できないので true。
/// @note 頂点走査もサーフェス交差も素で回すと 1 フレーム数十万頂点になる。当たらないメッシュは中身を見ずに捨てる。
bool RayHitsMeshBounds(const math::Ray& ray,
                       const renderer::Mesh& mesh,
                       const math::Matrix4& world,
                       const scene::Transform& tf,
                       float radiusInflate)
{
    if (mesh.boundsRadius <= 0.0f) return true;

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

/// @brief メッシュの CPU 頂点をワールド座標で 1 つずつ fn へ渡す。
/// @note cursorRay に当たらないメッシュは省く。数十 px 外れた縁の頂点も拾えるよう球は膨らませ、スキンはさらに倍にする。
/// @note 非表示スロットの submesh へは吸着させない。i はローカルスロット番号。
template<typename Fn>
void ForEachWorldVertex(scene::GameObject& go, const math::Ray& cursorRay, Fn&& fn)
{
    const math::Matrix4 world = go.transform.GetWorldMatrix();
    constexpr float kInflate = 1.25f;

    if (auto* mr = go.GetComponent<scene::MeshRenderer>(); mr && mr->mesh) {
        if (RayHitsMeshBounds(cursorRay, *mr->mesh, world, go.transform, kInflate))
            for (const auto& v : mr->mesh->cpuVertices)
                fn(TransformPoint(world, v.position));
    }
    if (auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>(); smr && smr->model) {
        const auto* mat = go.GetComponent<scene::MaterialComponent>();
        for (size_t i = 0; i < smr->SubmeshCount(); ++i) {
            const renderer::Mesh* meshPtr = smr->SubmeshMesh(i);
            if (!meshPtr) continue;
            if (mat && !mat->SlotAt(i).visible) continue;
            if (!RayHitsMeshBounds(cursorRay, *meshPtr, world, go.transform, kInflate * 2.0f))
                continue;
            for (const auto& v : meshPtr->cpuSkinnedVertices)
                fn(TransformPoint(world, v.position));
        }
    }
}

/// @brief カーソルに最も近い頂点をスクリーン空間で探す。
/// @param selectedSide true なら選択中から、false なら非選択から探す。
/// @return maxPixelDist 以内に無ければ false。outWorld は未変更。
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

/// @brief UP から normal への最小回転。
/// @note Quaternion に FromToRotation が無いので外積軸 + acos 角で組む。ほぼ同方向 / 真逆は外積が退化するので個別に扱う。
math::Quaternion AlignUpToNormal(const math::Vector3& normal)
{
    const math::Vector3 up = math::Vector3::UP;
    const math::Vector3 n  = normal.Normalized();
    const float d = math::Vector3::Dot(up, n);
    if (d >  0.9999f) return math::Quaternion::Identity();
    if (d < -0.9999f) return math::Quaternion::FromAxisAngle({ 1.0f, 0.0f, 0.0f }, math::PI);
    const math::Vector3 axis = math::Vector3::Cross(up, n).Normalized();
    return math::Quaternion::FromAxisAngle(axis, std::acos(d));
}

/// @brief 非選択オブジェクトのサーフェスへレイを飛ばす。
/// @return ヒットなしなら false。outPoint / outNormal はワールド空間。地形の法線は取れないので UP を返す。
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
        if (IsSelected(ctx, go.GetID())) continue;

        const math::Matrix4 world = go.transform.GetWorldMatrix();

        auto testMesh = [&](const renderer::Mesh& mesh, const auto& verts, const auto& indices,
                            float radiusInflate) {
            if (verts.empty() || indices.size() < 3) return;
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
            const auto* mat = go.GetComponent<scene::MaterialComponent>();
            for (size_t i = 0; i < smr->SubmeshCount(); ++i) {
                const renderer::Mesh* meshPtr = smr->SubmeshMesh(i);
                if (!meshPtr) continue;
                if (mat && !mat->SlotAt(i).visible) continue;
                testMesh(*meshPtr, meshPtr->cpuSkinnedVertices, meshPtr->cpuIndices, 2.0f);
            }
        }
    }

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
                outNormal = math::Vector3::UP;
                found     = true;
            }
        }
    }

    return found;
}

/// @brief 移動対象 (選択済みの祖先を持たない選択) を集め、Undo 用の before も記録する。
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

/// @brief 全対象をワールド空間で delta ぶん平行移動する。
/// @note ローカル position へ書くので、親があれば delta を親空間 (回転・スケール込み) へ戻してから足す。
void TranslateTargets(EditorContext& ctx, const math::Vector3& worldDelta)
{
    if (math::NearlyZero(worldDelta.x) && math::NearlyZero(worldDelta.y) && math::NearlyZero(worldDelta.z))
        return;
    for (const std::string& guid : g_drag.guids) {
        auto* target = ctx.activeScene->FindByGuid(guid);
        if (!target) continue;
        math::Vector3 localDelta = worldDelta;
        if (const scene::GameObject* parent = target->GetParent()) {
            const math::Vector4 d = math::Matrix4::Inverse(parent->transform.GetWorldMatrix())
                * math::Vector4{ worldDelta.x, worldDelta.y, worldDelta.z, 0.0f };
            localDelta = { d.x, d.y, d.z };
        }
        target->transform.position = target->transform.position + localDelta;
    }
}

/// @brief スナップ中のフィードバック (掴んだ点と吸着先)。
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

    /// @note V は押している間だけ有効 (Unity と同じモーメンタリ操作)。
    const bool vertexKey  = !io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_V) && !io.KeyCtrl;
    const bool surfaceKey = io.KeyCtrl && io.KeyShift;

    ctx.vertexSnapActive  = hasSelection && hovered && vertexKey;
    ctx.surfaceSnapActive = hasSelection && hovered && surfaceKey;

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

        /// @note 頂点スナップはカーソルに一番近い自分の頂点を掴む。見つからなければ原点を掴み、面スナップと同じ挙動になる。
        const math::Vector3 primaryWorld = CurrentWorldPosition(*primary);
        math::Vector3 grab = primaryWorld;
        if (g_drag.vertexMode) {
            constexpr float kGrabRadiusPx = 40.0f;
            math::Vector3   found{};
            if (FindNearestVertex(ctx, vpMin, vpSize, ImGui::GetMousePos(), true, kGrabRadiusPx, found))
                grab = found;
        }
        g_drag.grabWorld  = grab;
        g_drag.grabOffset = grab - primaryWorld;
        g_drag.active     = true;
    }

    if (!g_drag.active) return false;

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        PushSnapUndo(ctx, g_drag.vertexMode ? "Vertex Snap" : "Surface Snap");
        g_drag.active = false;
        /// @note このフレームはまだ他の操作へ渡さない。
        return true;
    }

    scene::GameObject* primary = ctx.activeScene->GetGameObject(ctx.PrimarySelected());
    if (!primary) { g_drag.active = false; return false; }

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

            /// @note 法線合わせは位置より先に回す。align はワールド回転なので、親があれば親のワールド回転を外してローカルへ書く。
            if (ctx.surfaceSnapAlignToNormal) {
                const math::Quaternion align = AlignUpToNormal(normal);
                for (const std::string& guid : g_drag.guids) {
                    auto* target = ctx.activeScene->FindByGuid(guid);
                    if (!target) continue;
                    const scene::GameObject* parent = target->GetParent();
                    target->transform.rotation = parent
                        ? parent->transform.worldRotation.Inverse() * align
                        : align;
                }
            }
        }
    }

    if (g_drag.hasTarget) {
        const math::Vector3 desiredPrimaryPos = g_drag.targetWorld - g_drag.grabOffset;
        const math::Vector3 delta = desiredPrimaryPos - CurrentWorldPosition(*primary);
        TranslateTargets(ctx, delta);
        g_drag.grabWorld = g_drag.targetWorld;
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    DrawSnapFeedback(ctx, vpMin, vpSize);
    return true;
}

} // namespace fbzz::editor
