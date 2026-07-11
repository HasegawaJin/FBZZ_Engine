// FBZZ Engine
// ViewportSceneGizmos.cpp | fbzz::editor
// Scene View のカメラ・ライトアイコンと3D Gizmo
#include "ViewportCommon.hpp"
#include <Editor/Util/UndoStack.hpp>

namespace fbzz::editor {

namespace {

bool GizmoTransformEquals(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.position.x == rhs.position.x &&
           lhs.position.y == rhs.position.y &&
           lhs.position.z == rhs.position.z &&
           lhs.rotation.x == rhs.rotation.x &&
           lhs.rotation.y == rhs.rotation.y &&
           lhs.rotation.z == rhs.rotation.z &&
           lhs.rotation.w == rhs.rotation.w &&
           lhs.scale.x == rhs.scale.x &&
           lhs.scale.y == rhs.scale.y &&
           lhs.scale.z == rhs.scale.z;
}

// worldRow (行優先ワールド行列) をローカル TRS に分解して transform へ書き戻す。
// ImGuizmo decomposes with Euler angles that do not match the engine quaternion convention.
// Extract TRS directly from the matrix to avoid handedness and sign mismatches.
void ApplyWorldRowToTransform(scene::GameObject& go, const math::Matrix4& worldRow)
{
    math::Matrix4 localRow = worldRow;
    if (scene::GameObject* parent = go.GetParent()) {
        const math::Matrix4 parentInv = math::Matrix4::Inverse(parent->transform.GetWorldMatrix());
        localRow = parentInv * worldRow;
    }

    const float sx = std::sqrt(localRow.m[0][0]*localRow.m[0][0] + localRow.m[1][0]*localRow.m[1][0] + localRow.m[2][0]*localRow.m[2][0]);
    const float sy = std::sqrt(localRow.m[0][1]*localRow.m[0][1] + localRow.m[1][1]*localRow.m[1][1] + localRow.m[2][1]*localRow.m[2][1]);
    const float sz = std::sqrt(localRow.m[0][2]*localRow.m[0][2] + localRow.m[1][2]*localRow.m[1][2] + localRow.m[2][2]*localRow.m[2][2]);

    math::Matrix4 rotMat = math::Matrix4::Identity();
    if (!math::NearlyZero(sx)) { rotMat.m[0][0] = localRow.m[0][0]/sx; rotMat.m[1][0] = localRow.m[1][0]/sx; rotMat.m[2][0] = localRow.m[2][0]/sx; }
    if (!math::NearlyZero(sy)) { rotMat.m[0][1] = localRow.m[0][1]/sy; rotMat.m[1][1] = localRow.m[1][1]/sy; rotMat.m[2][1] = localRow.m[2][1]/sy; }
    if (!math::NearlyZero(sz)) { rotMat.m[0][2] = localRow.m[0][2]/sz; rotMat.m[1][2] = localRow.m[1][2]/sz; rotMat.m[2][2] = localRow.m[2][2]/sz; }

    go.transform.position = { localRow.m[0][3], localRow.m[1][3], localRow.m[2][3] };
    go.transform.scale    = { sx, sy, sz };
    go.transform.rotation = math::Quaternion::FromMatrix4(rotMat);
}

} // namespace

bool WorldToScreen(const math::Vector3& world,
                   const EditorContext& ctx,
                   const ImVec2& vpMin, const ImVec2& vpSize,
                   ImVec2& out)
{
    if (!ctx.editorCamera) return false;
    const math::Matrix4 vp = ctx.editorCamera->GetProjectionMatrix()
                           * ctx.editorCamera->GetViewMatrix();
    const math::Vector4 clip = vp * math::Vector4{ world.x, world.y, world.z, 1.0f };
    if (clip.w <= 0.001f) return false;
    const float ndcX =  clip.x / clip.w;
    const float ndcY = -clip.y / clip.w;
    out = {
        vpMin.x + (ndcX * 0.5f + 0.5f) * vpSize.x,
        vpMin.y + (ndcY * 0.5f + 0.5f) * vpSize.y
    };
    return true;
}

void DrawDirectionLine(EditorContext& ctx,
                       ImDrawList* dl,
                       const math::Vector3& start,
                       const math::Vector3& direction,
                       float length,
                       const ImVec2& vpMin,
                       const ImVec2& vpSize,
                       ImU32 color)
{
    ImVec2 a;
    ImVec2 b;
    if (!WorldToScreen(start, ctx, vpMin, vpSize, a)) return;
    if (!WorldToScreen(start + direction.Normalized() * length, ctx, vpMin, vpSize, b)) return;

    dl->AddLine(a, b, color, 2.0f);

    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.001f) return;

    const float ux = dx / len;
    const float uy = dy / len;
    const ImVec2 left  = { b.x - ux * 9.0f - uy * 4.5f, b.y - uy * 9.0f + ux * 4.5f };
    const ImVec2 right = { b.x - ux * 9.0f + uy * 4.5f, b.y - uy * 9.0f - ux * 4.5f };
    dl->AddTriangleFilled(b, left, right, color);
}

void DrawCameraFrustum(EditorContext& ctx,
                       ImDrawList* dl,
                       const scene::Transform& transform,
                       const scene::CameraComponent& camera,
                       const ImVec2& vpMin,
                       const ImVec2& vpSize,
                       ImU32 color)
{
    const float nearDist = 0.45f;
    const float farDist = 2.5f;
    const float aspect = 16.0f / 9.0f;
    const float tanHalfFov = std::tanf(math::ToRad(camera.fovY) * 0.5f);

    const math::Vector3 origin = transform.position;
    const math::Vector3 forward = transform.forward.Normalized();
    const math::Vector3 right = transform.right.Normalized();
    const math::Vector3 up = transform.up.Normalized();

    auto corner = [&](float dist, float sx, float sy) {
        const float halfHeight = tanHalfFov * dist;
        const float halfWidth = halfHeight * aspect;
        return origin + forward * dist + right * (sx * halfWidth) + up * (sy * halfHeight);
    };

    const math::Vector3 nearCorners[4] = {
        corner(nearDist, -1.0f,  1.0f),
        corner(nearDist,  1.0f,  1.0f),
        corner(nearDist,  1.0f, -1.0f),
        corner(nearDist, -1.0f, -1.0f)
    };
    const math::Vector3 farCorners[4] = {
        corner(farDist, -1.0f,  1.0f),
        corner(farDist,  1.0f,  1.0f),
        corner(farDist,  1.0f, -1.0f),
        corner(farDist, -1.0f, -1.0f)
    };

    ImVec2 nearScreen[4];
    ImVec2 farScreen[4];
    bool nearVisible[4];
    bool farVisible[4];
    for (int i = 0; i < 4; ++i) {
        nearVisible[i] = WorldToScreen(nearCorners[i], ctx, vpMin, vpSize, nearScreen[i]);
        farVisible[i] = WorldToScreen(farCorners[i], ctx, vpMin, vpSize, farScreen[i]);
    }

    for (int i = 0; i < 4; ++i) {
        const int next = (i + 1) % 4;
        if (nearVisible[i] && nearVisible[next])
            dl->AddLine(nearScreen[i], nearScreen[next], color, 1.5f);
        if (farVisible[i] && farVisible[next])
            dl->AddLine(farScreen[i], farScreen[next], color, 1.5f);
        if (nearVisible[i] && farVisible[i])
            dl->AddLine(nearScreen[i], farScreen[i], color, 1.5f);
    }
}

void DrawSceneIcons(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize)
{
    if (!ctx.activeScene || !ctx.editorCamera) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 vpMax = { vpMin.x + vpSize.x, vpMin.y + vpSize.y };

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeSelf()) continue;

        ImVec2 sp;
        if (!WorldToScreen(go.transform.position, ctx, vpMin, vpSize, sp)) continue;
        if (sp.x < vpMin.x || sp.x > vpMax.x || sp.y < vpMin.y || sp.y > vpMax.y) continue;

        const bool isSelected = std::find(ctx.selectedEntities.begin(),
                                          ctx.selectedEntities.end(),
                                          go.GetID()) != ctx.selectedEntities.end();
        const ImU32 selCol = IM_COL32(255, 220, 60, 255);

        if (auto* light = go.GetComponent<scene::LightComponent>()) {
            // Light icon: center point with rays.
            constexpr float kR = 8.0f;
            constexpr float kRay = 14.0f;
            constexpr int   kRays = 8;
            const ImU32 col = isSelected ? selCol : IM_COL32(255, 200, 60, 200);
            dl->AddCircleFilled(sp, kR, col);
            dl->AddCircle(sp, kR, IM_COL32(0, 0, 0, 120), 16, 1.5f);
            for (int i = 0; i < kRays; ++i) {
                const float ang = static_cast<float>(i) * (2.0f * 3.14159265f / kRays);
                const ImVec2 a = { sp.x + std::cosf(ang) * (kR + 3.0f),
                                   sp.y + std::sinf(ang) * (kR + 3.0f) };
                const ImVec2 b = { sp.x + std::cosf(ang) * (kR + kRay),
                                   sp.y + std::sinf(ang) * (kR + kRay) };
                dl->AddLine(a, b, col, 1.5f);
            }
            if (light->enabled && light->type == scene::LightComponent::Type::Directional) {
                DrawDirectionLine(ctx, dl, go.transform.position, go.transform.forward,
                                  2.5f, vpMin, vpSize, col);
            }
        } else if (auto* camera = go.GetComponent<scene::CameraComponent>()) {
            // Camera icon: body rectangle and lens trapezoid.
            constexpr float kW = 16.0f, kH = 11.0f;
            constexpr float kLW = 7.0f, kLH = 5.0f, kLX = 9.0f;
            const ImU32 col  = isSelected ? selCol : IM_COL32(120, 200, 255, 200);
            const ImU32 dark = IM_COL32(0, 0, 0, 140);
            // Body.
            dl->AddRectFilled({ sp.x - kW, sp.y - kH * 0.5f },
                              { sp.x + kW * 0.4f, sp.y + kH * 0.5f }, col, 2.0f);
            dl->AddRect({ sp.x - kW, sp.y - kH * 0.5f },
                        { sp.x + kW * 0.4f, sp.y + kH * 0.5f }, dark, 2.0f);
            // Lens.
            ImVec2 lens[4] = {
                { sp.x + kW * 0.4f, sp.y - kLH },
                { sp.x + kLX,       sp.y - kLW  },
                { sp.x + kLX,       sp.y + kLW  },
                { sp.x + kW * 0.4f, sp.y + kLH  },
            };
            dl->AddConvexPolyFilled(lens, 4, col);
            dl->AddPolyline(lens, 4, dark, ImDrawFlags_Closed, 1.0f);
            DrawDirectionLine(ctx, dl, go.transform.position, go.transform.forward,
                              2.0f, vpMin, vpSize, col);
            if (camera->enabled)
                DrawCameraFrustum(ctx, dl, go.transform, *camera, vpMin, vpSize, col);
        }
    }
}

void DrawOrientationGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    if (!ctx.editorCamera) return;

    constexpr float kSize = 120.0f;
    const ImVec2 pos = { viewportMin.x + viewportSize.x - kSize - 8.0f, viewportMin.y + 8.0f };

    math::Matrix4 viewCol = ToColumnMajor(ctx.editorCamera->GetViewMatrix());

    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(viewportMin.x, viewportMin.y, viewportSize.x, viewportSize.y);

    const float dist = ctx.editorCamera->m_position.Length();
    ImGuizmo::ViewManipulate(
        &viewCol.m[0][0],
        (dist > 0.1f ? dist : 10.0f),
        pos, { kSize, kSize },
        0x40000000);

    if (!ImGuizmo::IsUsingViewManipulate()) return;

    // Convert column-major back to row-major.
    const math::Matrix4 viewRow = math::Matrix4::Transpose(viewCol);

    // Camera position: pos = -R^T * t. The 3x3 part of view is R^T.
    const float tx = viewRow.m[0][3], ty = viewRow.m[1][3], tz = viewRow.m[2][3];
    ctx.editorCamera->m_position = {
        -(viewRow.m[0][0]*tx + viewRow.m[1][0]*ty + viewRow.m[2][0]*tz),
        -(viewRow.m[0][1]*tx + viewRow.m[1][1]*ty + viewRow.m[2][1]*tz),
        -(viewRow.m[0][2]*tx + viewRow.m[1][2]*ty + viewRow.m[2][2]*tz)
    };

    // Camera rotation: rebuild the world rotation matrix from the view matrix.
    math::Matrix4 rotMat = math::Matrix4::Identity();
    rotMat.m[0][0] = viewRow.m[0][0]; rotMat.m[0][1] = viewRow.m[1][0]; rotMat.m[0][2] = viewRow.m[2][0];
    rotMat.m[1][0] = viewRow.m[0][1]; rotMat.m[1][1] = viewRow.m[1][1]; rotMat.m[1][2] = viewRow.m[2][1];
    rotMat.m[2][0] = viewRow.m[0][2]; rotMat.m[2][1] = viewRow.m[1][2]; rotMat.m[2][2] = viewRow.m[2][2];
    ctx.editorCamera->m_rotation = math::Quaternion::FromMatrix4(rotMat);
}

void DrawGizmo(EditorContext& ctx,
               const ImVec2& viewportMin,
               const ImVec2& viewportSize,
               int& lastOp,
               int& lastMode,
               bool& prevOver,
               bool& prevUsing)
{
    scene::EntityID selected = ctx.PrimarySelected();
    if (!selected.IsValid() || !ctx.activeScene || !ctx.editorCamera) return;

    scene::GameObject* go = ctx.activeScene->GetGameObject(selected);
    if (!go) return;

    HandleGizmoShortcuts(ctx);

    math::Matrix4 viewCol = ToColumnMajor(ctx.editorCamera->GetViewMatrix());
    math::Matrix4 projCol = ToColumnMajor(ctx.editorCamera->GetProjectionMatrix());
    math::Matrix4 worldCol = ToColumnMajor(go->transform.GetWorldMatrix());

    auto& style = ImGuizmo::GetStyle();
    style.Colors[ImGuizmo::SELECTION] = ImVec4(1.0f, 0.95f, 0.05f, 1.0f);
    style.Colors[ImGuizmo::INACTIVE] = ImVec4(0.75f, 0.75f, 0.75f, 0.75f);
    style.TranslationLineThickness = 4.0f;
    style.RotationLineThickness = 4.0f;
    style.ScaleLineThickness = 4.0f;

    ImGuizmo::SetDrawlist();
    ImGuizmo::Enable(true);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(viewportMin.x, viewportMin.y, viewportSize.x, viewportSize.y);

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (ctx.gizmoMode == EditorContext::GizmoMode::Rotate) op = ImGuizmo::ROTATE;
    if (ctx.gizmoMode == EditorContext::GizmoMode::Scale) op = ImGuizmo::SCALE;

    ImGuizmo::MODE mode = (ctx.gizmoSpace == EditorContext::GizmoSpace::World)
        ? ImGuizmo::WORLD
        : ImGuizmo::LOCAL;

    const float snapVal = (ctx.gizmoMode == EditorContext::GizmoMode::Rotate) ? ctx.snapRot
                        : (ctx.gizmoMode == EditorContext::GizmoMode::Scale)  ? ctx.snapScale
                        :                                                        ctx.snapPos;
    float snap[3] = { snapVal, snapVal, snapVal };
    const int opInt = static_cast<int>(op);
    const int modeInt = static_cast<int>(mode);
    if (lastOp != opInt || lastMode != modeInt) {
        lastOp = opInt;
        lastMode = modeInt;
    }

    // WHY: Unity と同じく Ctrl 押下中はモーメンタリスナップ (押している間だけスナップ有効)。
    //      設定でスナップ ON のときは常時有効。
    const bool snapActive = ctx.snapEnabled || ImGui::GetIO().KeyCtrl;
    ImGuizmo::Manipulate(
        &viewCol.m[0][0],
        &projCol.m[0][0],
        op,
        mode,
        &worldCol.m[0][0],
        nullptr,
        snapActive ? snap : nullptr);

    const bool gizmoOver = ImGuizmo::IsOver();
    const bool gizmoUsing = ImGuizmo::IsUsing();
    const bool wasUsing = prevUsing;

    // マルチ選択ドラッグ 1 回分の編集状態。instanceIds[0] は必ずプライマリ。
    // before / startWorld / applyDelta は instanceIds と同じ並び。
    struct GizmoEdit {
        std::vector<std::string> instanceIds;
        std::vector<scene::Transform> before;
        std::vector<math::Matrix4> startWorld;
        std::vector<bool> applyDelta;  // true: プライマリの移動量を相対適用する対象
        math::Matrix4 primaryStartWorldInv = math::Matrix4::Identity();
        EditorContext::GizmoMode mode = EditorContext::GizmoMode::Translate;
        bool active = false;
    };
    static GizmoEdit edit;

    if (gizmoUsing && !wasUsing) {
        edit.instanceIds.clear();
        edit.before.clear();
        edit.startWorld.clear();
        edit.applyDelta.clear();

        // WHY: 親子が同時に選択されている場合、子は親の移動に追従するため、
        //      子にも delta を掛けると二重に動く。「選択済みの祖先を持たない」
        //      top-level オブジェクトにだけ delta を適用する (Unity と同じ規則)。
        auto hasSelectedAncestor = [&ctx](scene::GameObject* obj) {
            for (scene::GameObject* p = obj->GetParent(); p; p = p->GetParent()) {
                if (std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(),
                              p->GetID()) != ctx.selectedEntities.end())
                    return true;
            }
            return false;
        };

        // プライマリを先頭に登録する (ギズモのワールド行列を直接書き込む対象)
        edit.instanceIds.push_back(go->instanceId);
        edit.before.push_back(go->transform);
        edit.startWorld.push_back(go->transform.GetWorldMatrix());
        edit.applyDelta.push_back(false);

        for (scene::EntityID id : ctx.selectedEntities) {
            if (id == selected) continue;
            scene::GameObject* sel = ctx.activeScene->GetGameObject(id);
            if (!sel || ctx.IsLocked(id)) continue;
            edit.instanceIds.push_back(sel->instanceId);
            edit.before.push_back(sel->transform);
            edit.startWorld.push_back(sel->transform.GetWorldMatrix());
            edit.applyDelta.push_back(!hasSelectedAncestor(sel));
        }

        edit.primaryStartWorldInv = math::Matrix4::Inverse(go->transform.GetWorldMatrix());
        edit.mode = ctx.gizmoMode;
        edit.active = true;
    }

    if (gizmoOver != prevOver || gizmoUsing != prevUsing) {
        prevOver = gizmoOver;
        prevUsing = gizmoUsing;
    }

    if (!gizmoUsing) {
        if (wasUsing && edit.active) {
            // ドラッグ終了: 全対象の before/after を 1 コマンドにまとめて Undo 登録する。
            // WHY: マルチ選択の移動を対象ごとに分けると Ctrl+Z を選択数だけ叩く羽目になる。
            scene::Scene* scene = ctx.activeScene;
            const std::vector<std::string> ids = edit.instanceIds;
            const std::vector<scene::Transform> before = edit.before;
            std::vector<scene::Transform> after;
            after.reserve(ids.size());
            bool anyChanged = false;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                scene::GameObject* obj = scene->FindByGuid(ids[i]);
                after.push_back(obj ? obj->transform : before[i]);
                if (obj && !GizmoTransformEquals(before[i], after[i]))
                    anyChanged = true;
            }
            const auto markDirty = ctx.markSceneDirty;
            const char* description =
                edit.mode == EditorContext::GizmoMode::Rotate ? "Rotate GameObject" :
                edit.mode == EditorContext::GizmoMode::Scale ? "Scale GameObject" :
                                                               "Move GameObject";
            if (ctx.undoStack && anyChanged) {
                auto applyAll = [scene, ids, markDirty](const std::vector<scene::Transform>& values) {
                    for (std::size_t i = 0; i < ids.size(); ++i)
                        if (auto* target = scene->FindByGuid(ids[i]))
                            target->transform = values[i];
                    if (markDirty) markDirty();
                };
                ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                    description,
                    [applyAll, after]() { applyAll(after); },
                    [applyAll, before]() { applyAll(before); }));
            }
            if (anyChanged && ctx.markSceneDirty)
                ctx.markSceneDirty();
            edit.active = false;
        }
        return;
    }

    const math::Matrix4 worldRow = math::Matrix4::Transpose(worldCol);

    // マルチ選択: プライマリの移動量 (delta) を他の top-level 選択へ相対適用する。
    // WHY: プライマリより先に他オブジェクトへ適用する。選択中の「親」が動いた後に
    //      プライマリのワールド行列を書き込むことで、プライマリは常にギズモ位置へ一致する。
    if (edit.active && edit.instanceIds.size() > 1) {
        const math::Matrix4 delta = worldRow * edit.primaryStartWorldInv;
        for (std::size_t i = 1; i < edit.instanceIds.size(); ++i) {
            if (!edit.applyDelta[i]) continue;
            if (auto* other = ctx.activeScene->FindByGuid(edit.instanceIds[i]))
                ApplyWorldRowToTransform(*other, delta * edit.startWorld[i]);
        }
    }

    ApplyWorldRowToTransform(*go, worldRow);
}

} // namespace fbzz::editor
