/// @file    ViewportSceneGizmos.cpp
/// @brief   Scene View のコンポーネントアイコン・選択物の範囲ワイヤー・3D Gizmo。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "ViewportCommon.hpp"
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/ViewportCamera.hpp>
#include <Engine/Scene/Components/AudioListenerComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/AudioSpatialComponents.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/PostProcessVolumeComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WeatherComponent.hpp>
#include <span>
#include <unordered_map>

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

/// @brief 行優先ワールド行列をローカル TRS に分解して transform へ書き戻す。
/// @note ImGuizmo の Euler 分解はエンジンのクォータニオン規約と符号・手系が合わないため、行列から直接 TRS を取り出す。
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

namespace {

/// @brief ワールド空間の矢印を描く。
/// @param direction ワールド方向。正規化は内部で行う。
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

/// @brief ワールド点列を折れ線で描く。カメラ背面に落ちた点に接する辺は描かない。
void DrawWorldPolyline(EditorContext& ctx, ImDrawList* dl,
                       std::span<const math::Vector3> points, bool closed,
                       const ImVec2& vpMin, const ImVec2& vpSize, ImU32 color, float thickness)
{
    if (points.size() < 2) return;
    ImVec2 first{};
    ImVec2 prev{};
    bool firstOk = false;
    bool prevOk  = false;
    for (size_t i = 0; i < points.size(); ++i) {
        ImVec2 cur;
        const bool ok = WorldToScreen(points[i], ctx, vpMin, vpSize, cur);
        if (ok && prevOk) dl->AddLine(prev, cur, color, thickness);
        if (i == 0) { first = cur; firstOk = ok; }
        prev = cur;
        prevOk = ok;
    }
    if (closed && points.size() > 2 && prevOk && firstOk)
        dl->AddLine(prev, first, color, thickness);
}

/// @brief center を中心に axisU / axisV が張る面の円を描く。
/// @param axisU 正規直交であること。
void DrawWorldCircle(EditorContext& ctx, ImDrawList* dl,
                     const math::Vector3& center, const math::Vector3& axisU, const math::Vector3& axisV,
                     float radius, const ImVec2& vpMin, const ImVec2& vpSize, ImU32 color, float thickness)
{
    constexpr int kSegments = 48;
    math::Vector3 points[kSegments];
    for (int i = 0; i < kSegments; ++i) {
        const float a = static_cast<float>(i) * (2.0f * math::PI / kSegments);
        points[i] = center + axisU * (std::cos(a) * radius) + axisV * (std::sin(a) * radius);
    }
    DrawWorldPolyline(ctx, dl, points, true, vpMin, vpSize, color, thickness);
}

/// @brief 色の不透明度を alpha 倍する。
ImU32 ScaleAlpha(ImU32 color, float alpha)
{
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFu) * math::Clamp(alpha, 0.0f, 1.0f);
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

/// @brief ワイヤー球 (軸 3 平面の円 + 視点から見た輪郭円)。
/// @note 輪郭は接線円 (中心を r²/d だけ手前へ寄せ、半径 r·√(d²-r²)/d) なので、軸の円より外へははみ出さない。
void DrawWireSphere(EditorContext& ctx, ImDrawList* dl, const math::Vector3& center, float radius,
                    const ImVec2& vpMin, const ImVec2& vpSize, ImU32 color)
{
    if (radius <= 1.0e-4f || !ctx.editorCamera) return;
    const ImU32 axisColor = ScaleAlpha(color, 0.45f);
    DrawWorldCircle(ctx, dl, center, math::Vector3::RIGHT, math::Vector3::FORWARD, radius, vpMin, vpSize, color, 1.5f);
    DrawWorldCircle(ctx, dl, center, math::Vector3::RIGHT, math::Vector3::UP,      radius, vpMin, vpSize, axisColor, 1.0f);
    DrawWorldCircle(ctx, dl, center, math::Vector3::UP,    math::Vector3::FORWARD, radius, vpMin, vpSize, axisColor, 1.0f);

    const bool ortho = ctx.editorCamera->m_projection == renderer::ProjectionMode::Orthographic;
    math::Vector3 toCamera = ortho ? ctx.editorCamera->GetForward() * -1.0f
                                   : ctx.editorCamera->m_position - center;
    const float d = toCamera.Length();
    if (d <= radius + 1.0e-3f) return;
    const math::Vector3 n = toCamera * (1.0f / d);
    math::Vector3 u = math::Vector3::Cross(n, math::Vector3::UP);
    if (u.LengthSq() < 1.0e-6f) u = math::Vector3::Cross(n, math::Vector3::RIGHT);
    u = u.Normalized();
    const math::Vector3 v = math::Vector3::Cross(n, u).Normalized();
    const math::Vector3 rimCenter = ortho ? center : center + n * (radius * radius / d);
    const float rimRadius = ortho ? radius : radius * std::sqrt(d * d - radius * radius) / d;
    DrawWorldCircle(ctx, dl, rimCenter, u, v, rimRadius, vpMin, vpSize, color, 1.5f);
}

/// @brief ワールド軸に沿った箱。
/// @param halfExtents 各軸の半径 [m]。
void DrawWireAabb(EditorContext& ctx, ImDrawList* dl, const math::Vector3& center, const math::Vector3& halfExtents,
                  const ImVec2& vpMin, const ImVec2& vpSize, ImU32 color)
{
    const auto corner = [&](int i) {
        return math::Vector3{ center.x + ((i & 1) ? halfExtents.x : -halfExtents.x),
                              center.y + ((i & 2) ? halfExtents.y : -halfExtents.y),
                              center.z + ((i & 4) ? halfExtents.z : -halfExtents.z) };
    };
    static constexpr int kEdges[12][2] = {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };
    for (const auto& edge : kEdges) {
        const math::Vector3 seg[2] = { corner(edge[0]), corner(edge[1]) };
        DrawWorldPolyline(ctx, dl, seg, false, vpMin, vpSize, color, 1.5f);
    }
}

/// @brief SplineComponent のローカル空間 Catmull-Rom 評価。
/// @note SplineSystem (GameplayComponentSystems.cpp の CatmullRom) と同じ式。あちらは非公開なので複製している。
math::Vector3 EvaluateSplineLocal(const scene::SplineComponent& spline, float normalized)
{
    const int count = static_cast<int>(spline.points.size());
    if (count == 0) return math::Vector3::ZERO;
    if (count == 1) return spline.points.front();
    const int segmentCount = spline.closed ? count : count - 1;
    const float scaled = math::Clamp(normalized, 0.0f, 1.0f) * static_cast<float>(segmentCount);
    const int segment = (std::min)(static_cast<int>(scaled), segmentCount - 1);
    const float t = scaled - static_cast<float>(segment);
    const auto point = [&](int index) -> const math::Vector3& {
        if (spline.closed) {
            index = (index % count + count) % count;
            return spline.points[static_cast<size_t>(index)];
        }
        return spline.points[static_cast<size_t>(std::clamp(index, 0, count - 1))];
    };
    const math::Vector3& p0 = point(segment - 1);
    const math::Vector3& p1 = point(segment);
    const math::Vector3& p2 = point(segment + 1);
    const math::Vector3& p3 = point(segment + 2);
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t
        + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2
        + (-p0 + p1 * 3.0f - p2 * 3.0f + p3) * t3) * 0.5f;
}

math::Vector3 TransformPoint(const math::Matrix4& world, const math::Vector3& p)
{
    const math::Vector4 v = world * math::Vector4{ p.x, p.y, p.z, 1.0f };
    return { v.x, v.y, v.z };
}

/// @brief カメラの視錐台を描く。
/// @note farZ は 1000m 級が普通で、そのまま描くと画面を線が横切るだけになる。表示上の遠平面は kFrustumDisplayFar で打ち切る。
/// @note CameraComponent は透視投影しか持たない (正投影は editor の renderer::Camera だけ)。
void DrawCameraFrustum(EditorContext& ctx,
                       ImDrawList* dl,
                       const scene::Transform& transform,
                       const scene::CameraComponent& camera,
                       const ImVec2& vpMin,
                       const ImVec2& vpSize,
                       ImU32 color)
{
    constexpr float kFrustumDisplayFar = 10.0f;
    const float nearDist = (std::max)(camera.nearZ, 0.001f);
    const float farDist  = (std::max)((std::min)(camera.farZ, kFrustumDisplayFar), nearDist + 0.01f);
    const bool  farClipped = camera.farZ > kFrustumDisplayFar;
    const float aspect = camera.aspectRatio > 1.0e-3f ? camera.aspectRatio : 16.0f / 9.0f;
    const float tanHalfFov = std::tan(math::ToRad(math::Clamp(camera.fovY, 1.0f, 179.0f)) * 0.5f);

    const math::Vector3 origin = transform.worldPosition;
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

    /// @note 打ち切った遠平面は薄く描き、実際の farZ ではないことを見分けられるようにする。
    const ImU32 farColor = farClipped ? ScaleAlpha(color, 0.35f) : color;
    for (int i = 0; i < 4; ++i) {
        const int next = (i + 1) % 4;
        if (nearVisible[i] && nearVisible[next])
            dl->AddLine(nearScreen[i], nearScreen[next], color, 1.5f);
        if (farVisible[i] && farVisible[next])
            dl->AddLine(farScreen[i], farScreen[next], farColor, 1.5f);
        if (nearVisible[i] && farVisible[i])
            dl->AddLine(nearScreen[i], farScreen[i], color, 1.5f);
    }
}

/// @brief ワールド座標をスクリーンへ投影し、視錐台の外なら false を返す。
/// @note NDC の z も見て、遠平面より奥のアイコンを描かない (DirectX の NDC は z ∈ [0,1])。
bool ProjectIcon(const math::Vector3& world,
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
    const float ndcZ =  clip.z / clip.w;
    if (ndcZ < 0.0f || ndcZ > 1.0f) return false;
    constexpr float kMargin = 24.0f;
    const float sx = vpMin.x + (ndcX * 0.5f + 0.5f) * vpSize.x;
    const float sy = vpMin.y + (ndcY * 0.5f + 0.5f) * vpSize.y;
    if (sx < vpMin.x - kMargin || sx > vpMin.x + vpSize.x + kMargin ||
        sy < vpMin.y - kMargin || sy > vpMin.y + vpSize.y + kMargin) return false;

    out = { sx, sy };
    return true;
}

enum class IconShape : uint8_t { Light, Camera, Badge };

/// @brief アイコン種別 1 行。描画・ピッキング・Overlays のトグルがすべてこの表を引く。
struct SceneIconDef {
    const char* key;
    const char* label;
    const char* glyph;
    ImU32       color;
    IconShape   shape;
    std::span<const scene::EntityID> (*entities)(const scene::Scene&);
};

template<typename T>
std::span<const scene::EntityID> EntitiesOf(const scene::Scene& owner)
{
    return owner.GetEntities<T>();
}

/// @note メッシュを持たないコンポーネントは Hierarchy からしか選べず配置も見えないので、色と頭文字のバッジで出す。
const SceneIconDef kSceneIcons[] = {
    { "light",                "Light",                nullptr, IM_COL32(255, 200,  60, 200), IconShape::Light,  &EntitiesOf<scene::LightComponent> },
    { "camera",               "Camera",               nullptr, IM_COL32(120, 200, 255, 200), IconShape::Camera, &EntitiesOf<scene::CameraComponent> },
    { "virtual_camera",       "Virtual Camera",       "VC",    IM_COL32(120, 170, 255, 210), IconShape::Badge,  &EntitiesOf<scene::VirtualCameraComponent> },
    { "audio_source",         "Audio Source",         "A",     IM_COL32(140, 220, 150, 210), IconShape::Badge,  &EntitiesOf<scene::AudioSourceComponent> },
    { "audio_listener",       "Audio Listener",       "L",     IM_COL32(110, 200, 200, 210), IconShape::Badge,  &EntitiesOf<scene::AudioListenerComponent> },
    { "audio_reverb_zone",    "Audio Reverb Zone",    "Rv",    IM_COL32(120, 210, 180, 210), IconShape::Badge,  &EntitiesOf<scene::AudioReverbZoneComponent> },
    { "particle_emitter",     "Particle Emitter",     "P",     IM_COL32(230, 150, 230, 210), IconShape::Badge,  &EntitiesOf<scene::ParticleEmitter> },
    { "flow_field",           "Flow Field",           "F",     IM_COL32(200, 120, 240, 210), IconShape::Badge,  &EntitiesOf<scene::FlowField> },
    { "physics_volume",       "Physics Volume",       "V",     IM_COL32(110, 160, 255, 210), IconShape::Badge,  &EntitiesOf<scene::VolumeComponent> },
    { "nav_mesh_agent",       "NavMesh Agent",        "N",     IM_COL32(120, 190, 120, 210), IconShape::Badge,  &EntitiesOf<scene::NavMeshAgentComponent> },
    { "character_controller", "Character Controller", "CC",    IM_COL32(170, 200, 110, 210), IconShape::Badge,  &EntitiesOf<scene::CharacterControllerComponent> },
    { "joint",                "Joint",                "J",     IM_COL32(230, 200, 130, 210), IconShape::Badge,  &EntitiesOf<scene::JointComponent> },
    { "spline",               "Spline",               "S",     IM_COL32(255, 150, 110, 210), IconShape::Badge,  &EntitiesOf<scene::SplineComponent> },
    { "reflection_probe",     "Reflection Probe",     "R",     IM_COL32(190, 190, 240, 210), IconShape::Badge,  &EntitiesOf<scene::ReflectionProbeComponent> },
    { "decal",                "Decal",                "D",     IM_COL32(240, 180, 120, 210), IconShape::Badge,  &EntitiesOf<scene::DecalComponent> },
    { "environment_light",    "Environment Light",    "E",     IM_COL32(250, 230, 150, 210), IconShape::Badge,  &EntitiesOf<scene::EnvironmentLightComponent> },
    { "post_process_volume",  "Post Process Volume",  "PP",    IM_COL32(220, 140, 180, 210), IconShape::Badge,  &EntitiesOf<scene::PostProcessVolumeComponent> },
    { "weather",              "Weather",              "W",     IM_COL32(150, 190, 230, 210), IconShape::Badge,  &EntitiesOf<scene::WeatherComponent> },
    { "volumetric_cloud",     "Volumetric Cloud",     "C",     IM_COL32(210, 215, 230, 210), IconShape::Badge,  &EntitiesOf<scene::VolumetricCloudComponent> },
};

constexpr int kSceneIconCount = static_cast<int>(sizeof(kSceneIcons) / sizeof(kSceneIcons[0]));

constexpr ImU32 kSelectedColor        = IM_COL32(255, 220, 60, 255);
/// @note これより遠いアイコンは描かない (選択中は除く)。遠景に数百個並ぶと手前の操作対象が埋もれる。
constexpr float kIconMaxDistance      = 200.0f;
/// @note この距離までは原寸。以遠は距離に反比例して kIconMinScale まで縮める。
constexpr float kIconFullSizeDistance = 15.0f;
constexpr float kIconMinScale         = 0.55f;
/// @note 最大距離の手前この割合からフェードを始める。
constexpr float kIconFadeStart        = 0.75f;
constexpr float kIconStackOffset      = 22.0f;
constexpr float kIconHitRadius        = 14.0f;

/// @brief バッジアイコン (角丸の四角 + 1〜2 文字)。
void DrawBadgeIcon(ImDrawList* dl, const SceneIconInstance& icon, const char* glyph, ImU32 color, bool selected)
{
    const float fontSize = ImGui::GetFontSize() * icon.scale;
    const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, glyph);
    const float halfH = 9.0f * icon.scale;
    const float halfW = (std::max)(halfH, textSize.x * 0.5f + 3.0f * icon.scale);
    const ImVec2 a = { icon.screenPos.x - halfW, icon.screenPos.y - halfH };
    const ImVec2 b = { icon.screenPos.x + halfW, icon.screenPos.y + halfH };

    dl->AddRectFilled(a, b, color, 4.0f * icon.scale);
    dl->AddRect(a, b, IM_COL32(0, 0, 0, static_cast<int>(140 * icon.alpha)), 4.0f * icon.scale, 0, 1.5f);
    if (selected)
        dl->AddRect({ a.x - 3.0f, a.y - 3.0f }, { b.x + 3.0f, b.y + 3.0f }, kSelectedColor, 6.0f, 0, 2.0f);

    dl->AddText(ImGui::GetFont(), fontSize,
                { icon.screenPos.x - textSize.x * 0.5f, icon.screenPos.y - textSize.y * 0.5f },
                IM_COL32(15, 15, 15, static_cast<int>(230 * icon.alpha)), glyph);
}

void DrawLightIcon(ImDrawList* dl, const SceneIconInstance& icon, ImU32 col)
{
    const float r   = 8.0f * icon.scale;
    const float ray = 14.0f * icon.scale;
    constexpr int kRays = 8;
    dl->AddCircleFilled(icon.screenPos, r, col);
    dl->AddCircle(icon.screenPos, r, IM_COL32(0, 0, 0, static_cast<int>(120 * icon.alpha)), 16, 1.5f);
    for (int i = 0; i < kRays; ++i) {
        const float ang = static_cast<float>(i) * (2.0f * math::PI / kRays);
        const ImVec2 a = { icon.screenPos.x + std::cos(ang) * (r + 3.0f * icon.scale),
                           icon.screenPos.y + std::sin(ang) * (r + 3.0f * icon.scale) };
        const ImVec2 b = { icon.screenPos.x + std::cos(ang) * (r + ray),
                           icon.screenPos.y + std::sin(ang) * (r + ray) };
        dl->AddLine(a, b, col, 1.5f);
    }
}

void DrawCameraIcon(ImDrawList* dl, const SceneIconInstance& icon, ImU32 col)
{
    const float s  = icon.scale;
    const float kW = 16.0f * s, kH = 11.0f * s;
    const float kLW = 7.0f * s, kLH = 5.0f * s, kLX = 9.0f * s;
    const ImU32 dark = IM_COL32(0, 0, 0, static_cast<int>(140 * icon.alpha));
    const ImVec2 sp  = icon.screenPos;

    dl->AddRectFilled({ sp.x - kW, sp.y - kH * 0.5f }, { sp.x + kW * 0.4f, sp.y + kH * 0.5f }, col, 2.0f);
    dl->AddRect({ sp.x - kW, sp.y - kH * 0.5f }, { sp.x + kW * 0.4f, sp.y + kH * 0.5f }, dark, 2.0f);
    ImVec2 lens[4] = {
        { sp.x + kW * 0.4f, sp.y - kLH },
        { sp.x + kLX,       sp.y - kLW },
        { sp.x + kLX,       sp.y + kLW },
        { sp.x + kW * 0.4f, sp.y + kLH },
    };
    dl->AddConvexPolyFilled(lens, 4, col);
    dl->AddPolyline(lens, 4, dark, ImDrawFlags_Closed, 1.0f);
}

/// @brief 選択中の水面の範囲・実効波・水流を描く。
/// @note .mat の値ではなく倍率と環境風を掛けた実効波を描く。そうでないと風を置いても何が変わったか見えない。
void DrawWaterGizmo(EditorContext& ctx, ImDrawList* dl, const scene::GameObject& go,
                    const scene::WaterComponent& water, const ImVec2& vpMin, const ImVec2& vpSize)
{
    const math::Matrix4 world = go.transform.GetWorldMatrix();
    const float hx = water.extentX * 0.5f;
    const float hz = water.extentZ * 0.5f;
    const math::Vector3 corners[4] = {
        TransformPoint(world, { -hx, 0.0f, -hz }), TransformPoint(world, { hx, 0.0f, -hz }),
        TransformPoint(world, {  hx, 0.0f,  hz }), TransformPoint(world, { -hx, 0.0f, hz }),
    };
    DrawWorldPolyline(ctx, dl, corners, true, vpMin, vpSize, IM_COL32(80, 200, 255, 200), 1.5f);

    /// @note 色は .mat Inspector の Wave 0..3 と揃える。長さは振幅に比例させ、水面の外へは出さない。
    static constexpr ImU32 kWaveColors[4] = {
        IM_COL32(255, 230,  80, 230), IM_COL32(255, 160,  80, 220),
        IM_COL32( 80, 255, 160, 220), IM_COL32(200,  80, 255, 220),
    };
    const math::Vector3 origin = go.transform.worldPosition;
    const float maxLength = (std::max)(
        (std::min)(hx * std::abs(go.transform.worldScale.x), hz * std::abs(go.transform.worldScale.z)) * 0.8f,
        0.5f);
    if (water.enableGerstnerWaves) {
        for (size_t i = 0; i < water.waves.size() && i < 4; ++i) {
            const scene::GerstnerWave& wave = water.waves[i];
            if (wave.amplitude < 1.0e-4f || wave.direction.LengthSq() < 1.0e-8f) continue;
            const math::Vector2 dir = wave.direction.Normalized();
            DrawDirectionLine(ctx, dl, origin, { dir.x, 0.0f, dir.y },
                              (std::min)(wave.amplitude * 20.0f, maxLength), vpMin, vpSize, kWaveColors[i]);
        }
    }
    if (const float currentSpeed = water.current.Length(); currentSpeed > 1.0e-3f) {
        DrawDirectionLine(ctx, dl, origin,
                          { water.current.x / currentSpeed, 0.0f, water.current.y / currentSpeed },
                          (std::min)(currentSpeed * 4.0f, maxLength), vpMin, vpSize,
                          IM_COL32(90, 255, 230, 230));
    }
}

/// @brief 選択物の «見えない体積» を描く。
/// @note デカール箱・拘束・骨・VFX 形状・物理形状は Engine 側のデバッグパスが描くので、ここでは扱わない。
/// @note CharacterController は半径/高さを持たず (形状は Collider)、NavMeshModifier も体積を持たない (Collider を修飾する) ので描くものが無い。
void DrawSelectedVolumes(EditorContext& ctx, ImDrawList* dl, scene::GameObject& go,
                         const ImVec2& vpMin, const ImVec2& vpSize)
{
    const math::Vector3 pos = go.transform.worldPosition;

    if (const auto* probe = go.GetComponent<scene::ReflectionProbeComponent>()) {
        const ImU32 col = IM_COL32(190, 190, 240, 220);
        /// @note ReflectionProbeCapturePass は箱をワールド軸・回転無視・スケール無視で判定する。
        if (probe->boxInfluence) DrawWireAabb(ctx, dl, pos, probe->boxExtents, vpMin, vpSize, col);
        else                     DrawWireSphere(ctx, dl, pos, probe->influenceRadius, vpMin, vpSize, col);
    }

    if (const auto* audio = go.GetComponent<scene::AudioSourceComponent>()) {
        /// @note spatialBlend=0 (完全 2D) では距離が効かないので薄く描く。
        const float a = audio->spatialBlend > 0.0f ? 1.0f : 0.35f;
        DrawWireSphere(ctx, dl, pos, audio->minDistance, vpMin, vpSize, ScaleAlpha(IM_COL32(140, 220, 150, 230), a));
        DrawWireSphere(ctx, dl, pos, audio->maxDistance, vpMin, vpSize, ScaleAlpha(IM_COL32(140, 220, 150, 150), a));
    }

    if (const auto* reverb = go.GetComponent<scene::AudioReverbZoneComponent>()) {
        DrawWireSphere(ctx, dl, pos, reverb->innerRadius, vpMin, vpSize, IM_COL32(120, 210, 180, 230));
        DrawWireSphere(ctx, dl, pos, reverb->outerRadius, vpMin, vpSize, IM_COL32(120, 210, 180, 150));
    }

    if (const auto* volume = go.GetComponent<scene::PostProcessVolumeComponent>(); volume && !volume->isGlobal) {
        const ImU32 col = IM_COL32(220, 140, 180, 220);
        DrawWireSphere(ctx, dl, pos, volume->influenceRadius, vpMin, vpSize, col);
        const float inner = volume->influenceRadius - volume->blendDistance;
        if (volume->blendDistance > 0.0f && inner > 0.0f)
            DrawWireSphere(ctx, dl, pos, inner, vpMin, vpSize, ScaleAlpha(col, 0.5f));
    }

    if (const auto* agent = go.GetComponent<scene::NavMeshAgentComponent>()) {
        /// @note Agent は高さを持たない (NavMeshSurface 側のベイク設定) ので足元の半径だけ描く。
        DrawWorldCircle(ctx, dl, pos, math::Vector3::RIGHT, math::Vector3::FORWARD, agent->radius,
                        vpMin, vpSize, IM_COL32(120, 190, 120, 230), 2.0f);
    }

    if (const auto* spline = go.GetComponent<scene::SplineComponent>(); spline && !spline->points.empty()) {
        const math::Matrix4 world = go.transform.GetWorldMatrix();
        const int count = static_cast<int>(spline->points.size());
        const int segmentCount = spline->closed ? count : count - 1;
        const ImU32 col = IM_COL32(255, 150, 110, 230);
        if (segmentCount > 0) {
            constexpr int kSamplesPerSegment = 16;
            constexpr int kMaxSamples = 1024;
            const int samples = (std::min)(segmentCount * kSamplesPerSegment, kMaxSamples);
            std::vector<math::Vector3> curve;
            curve.reserve(static_cast<size_t>(samples) + 1);
            for (int i = 0; i <= samples; ++i)
                curve.push_back(TransformPoint(world,
                    EvaluateSplineLocal(*spline, static_cast<float>(i) / static_cast<float>(samples))));
            DrawWorldPolyline(ctx, dl, curve, false, vpMin, vpSize, col, 2.0f);
        }
        for (const math::Vector3& p : spline->points) {
            ImVec2 sp;
            if (WorldToScreen(TransformPoint(world, p), ctx, vpMin, vpSize, sp)) {
                dl->AddCircleFilled(sp, 3.5f, col);
                dl->AddCircle(sp, 3.5f, IM_COL32(0, 0, 0, 160), 10, 1.0f);
            }
        }
    }

    if (const auto* water = go.GetComponent<scene::WaterComponent>())
        DrawWaterGizmo(ctx, dl, go, *water, vpMin, vpSize);
}

} // namespace

int SceneIconTypeCount() { return kSceneIconCount; }

const char* SceneIconTypeKey(int type)
{
    return (type >= 0 && type < kSceneIconCount) ? kSceneIcons[type].key : "";
}

const char* SceneIconTypeLabel(int type)
{
    return (type >= 0 && type < kSceneIconCount) ? kSceneIcons[type].label : "";
}

bool IsSceneIconTypeVisible(const EditorContext& ctx, int type)
{
    const std::string_view key = SceneIconTypeKey(type);
    return std::find(ctx.hiddenSceneIcons.begin(), ctx.hiddenSceneIcons.end(), key)
        == ctx.hiddenSceneIcons.end();
}

void SetSceneIconTypeVisible(EditorContext& ctx, int type, bool visible)
{
    if (type < 0 || type >= kSceneIconCount) return;
    const std::string_view key = SceneIconTypeKey(type);
    auto& hidden = ctx.hiddenSceneIcons;
    const auto it = std::find(hidden.begin(), hidden.end(), key);
    if (visible && it != hidden.end()) hidden.erase(it);
    else if (!visible && it == hidden.end()) hidden.emplace_back(key);
}

float SceneIconHitRadius(const SceneIconInstance& icon)
{
    return (std::max)(kIconHitRadius * icon.scale, 8.0f);
}

void CollectSceneIcons(const EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize,
                       std::vector<SceneIconInstance>& out)
{
    out.clear();
    if (!ctx.showSceneIcons || !ctx.activeScene || !ctx.editorCamera) return;

    const bool ortho = ctx.editorCamera->m_projection == renderer::ProjectionMode::Orthographic;
    const math::Vector3 cameraPos = ctx.editorCamera->m_position;
    const auto isSelected = [&ctx](scene::EntityID id) {
        return std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id)
            != ctx.selectedEntities.end();
    };

    std::unordered_map<uint64_t, int> stacked;
    for (int type = 0; type < kSceneIconCount; ++type) {
        if (!IsSceneIconTypeVisible(ctx, type)) continue;
        /// @note GetEntities<T>() で持ち主だけを引く。VFX Graph は 1 エフェクトでノード数ぶんの GO を作るので全 GO 走査は桁で重い。
        for (const scene::EntityID id : kSceneIcons[type].entities(*ctx.activeScene)) {
            const scene::GameObject* go = ctx.activeScene->GetGameObject(id);
            if (!go) continue;

            const math::Vector3& world = go->transform.worldPosition;
            float scale = 1.0f;
            float alpha = 1.0f;
            /// @note 正投影は距離で見かけの大きさが変わらないので、縮小もフェードもしない。
            if (!ortho && !isSelected(id)) {
                const float dist = (world - cameraPos).Length();
                if (dist > kIconMaxDistance) continue;
                scale = math::Clamp(kIconFullSizeDistance / (std::max)(dist, 1.0e-3f), kIconMinScale, 1.0f);
                const float fadeStart = kIconMaxDistance * kIconFadeStart;
                if (dist > fadeStart)
                    alpha = 1.0f - (dist - fadeStart) / (kIconMaxDistance - fadeStart);
            }

            ImVec2 sp;
            if (!ProjectIcon(world, ctx, vpMin, vpSize, sp)) continue;

            int& order = stacked[(static_cast<uint64_t>(id.index) << 32) | id.generation];
            sp.x += static_cast<float>(order) * kIconStackOffset * scale;
            ++order;

            /// @note 非アクティブを隠すとビューポートから選び直せなくなるので、薄くして残す。
            const bool active = go->activeInHierarchy();
            if (!active) alpha *= 0.35f;

            SceneIconInstance icon;
            icon.id        = id;
            icon.type      = type;
            icon.screenPos = sp;
            icon.scale     = scale;
            icon.alpha     = alpha;
            icon.active    = active;
            out.push_back(icon);
        }
    }
}

void DrawSceneIcons(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize)
{
    if (!ctx.activeScene || !ctx.editorCamera) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto isSelected = [&ctx](scene::EntityID id) {
        return std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id)
            != ctx.selectedEntities.end();
    };

    static std::vector<SceneIconInstance> icons;
    CollectSceneIcons(ctx, vpMin, vpSize, icons);

    for (const SceneIconInstance& icon : icons) {
        scene::GameObject* go = ctx.activeScene->GetGameObject(icon.id);
        if (!go) continue;
        const SceneIconDef& def = kSceneIcons[icon.type];
        const bool selected = isSelected(icon.id);
        const ImU32 color = ScaleAlpha(def.color, icon.alpha);

        switch (def.shape) {
        case IconShape::Light: {
            const auto* light = go->GetComponent<scene::LightComponent>();
            if (!light) break;
            const ImU32 col = selected ? kSelectedColor : color;
            DrawLightIcon(dl, icon, col);
            /// @note 範囲・形状のワイヤーは Overlay の "Light Range" (LightRangeDebugPass) が描く。ここでも描くと二重線になる。
            if (icon.active && light->enabled && light->type == scene::LightComponent::Type::Directional)
                DrawDirectionLine(ctx, dl, go->transform.worldPosition, go->transform.forward,
                                  2.5f, vpMin, vpSize, col);
            break;
        }
        case IconShape::Camera: {
            const auto* camera = go->GetComponent<scene::CameraComponent>();
            if (!camera) break;
            const ImU32 col = selected ? kSelectedColor : color;
            DrawCameraIcon(dl, icon, col);
            DrawDirectionLine(ctx, dl, go->transform.worldPosition, go->transform.forward,
                              2.0f, vpMin, vpSize, col);
            if (icon.active && camera->enabled)
                DrawCameraFrustum(ctx, dl, go->transform, *camera, vpMin, vpSize, col);
            break;
        }
        case IconShape::Badge:
            DrawBadgeIcon(dl, icon, def.glyph, color, selected);
            break;
        }
    }

    /// @note 範囲ワイヤーは選択中だけ。水面や音の最大距離は広く、常時出すと編集中ずっと視界を横切る。
    for (const scene::EntityID id : ctx.selectedEntities) {
        if (scene::GameObject* go = ctx.activeScene->GetGameObject(id))
            DrawSelectedVolumes(ctx, dl, *go, vpMin, vpSize);
    }
}

namespace {

/// @note ナビゲーションギズモ (Blender / Godot 風の軸ボール)。ImGuizmo::ViewManipulate の立方体は向きもクリック先も読み取りづらかったので置き換えた。
/// @note 操作は «ボールをクリック = その軸の視点へ回り込む» と «ギズモ内ドラッグ = オービット» の 2 つだけ。

constexpr float kNavSize      = 104.0f;  ///< ギズモ全体の一辺 [px]
constexpr float kNavPadding   = 10.0f;   ///< ビューポート右上からの余白 [px]
constexpr float kNavBallNearR = 11.0f;   ///< 手前側のボール半径 [px]
constexpr float kNavBallFarR  = 7.5f;    ///< 奥側のボール半径 [px]
constexpr float kNavSnapTime  = 0.26f;   ///< 軸クリック時の視点移動 [s]
constexpr float kNavDragSlop  = 4.0f;    ///< クリックとドラッグを分けるしきい値 [px]
constexpr float kNavOrbitSens = 0.45f;   ///< ドラッグ 1px あたりの回転 [deg]

/// @brief 6 方向の定義。描画・当たり判定はこの並びの添字で参照する。
struct NavAxis {
    math::Vector3 dir;
    int           colorIndex;
    const char*   label;
    bool          positive;    ///< 正方向だけ中心から線を引き、ラベルを常時出す
};

const NavAxis kNavAxes[6] = {
    { {  1.0f,  0.0f,  0.0f }, 0, "X", true  },
    { { -1.0f,  0.0f,  0.0f }, 0, "X", false },
    { {  0.0f,  1.0f,  0.0f }, 1, "Y", true  },
    { {  0.0f, -1.0f,  0.0f }, 1, "Y", false },
    { {  0.0f,  0.0f,  1.0f }, 2, "Z", true  },
    { {  0.0f,  0.0f, -1.0f }, 2, "Z", false },
};

const ImU32 kNavAxisColor[3] = {
    IM_COL32(238,  86, 100, 255),
    IM_COL32(150, 208,  72, 255),
    IM_COL32( 74, 144, 236, 255),
};

/// @brief フレームをまたぐ操作状態。
/// @note ViewportPanel が «ギズモがマウスを取っているか» を前フレームの結果で問い合わせるため静的に持つ。
struct NavGizmoState {
    bool   hovered     = false;
    int    hoveredAxis = -1;
    bool   pressed     = false;
    bool   dragging    = false;  ///< しきい値を超えて回した (クリック扱いにしない)
    int    pressedAxis = -1;
    ImVec2 pressPos{};
    float  orbitYaw    = 0.0f;   ///< ドラッグ中に積む yaw [deg]
    float  orbitPitch  = 0.0f;   ///< ドラッグ中に積む pitch [deg]

    /// @note 補間は Slerp でなく yaw/pitch で行う。DebugCamera は yaw/pitch から姿勢を組むので、ロールが混ざると着地時に水平がずれる。
    bool  animActive = false;
    float animT      = 0.0f;
    float fromYaw = 0.0f, fromPitch = 0.0f;
    float toYaw   = 0.0f, toPitch   = 0.0f;
};

NavGizmoState g_navGizmo;

/// @brief 明度と不透明度をまとめて調整する (奥行きフェードとホバー強調)。
ImU32 NavShade(ImU32 color, float brightness, float alpha)
{
    const auto ch = [&](int shift) {
        const float v = static_cast<float>((color >> shift) & 0xFFu) * brightness;
        return static_cast<ImU32>(math::Clamp(v, 0.0f, 255.0f));
    };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT),
                    static_cast<ImU32>(math::Clamp(alpha * 255.0f, 0.0f, 255.0f)));
}

/// @brief forward からエンジン規約の yaw / pitch [deg] を取り出す。
/// @note DebugCamera::Teleport と同じ式でないと、適用後に内部 yaw/pitch がずれて次のオービットで視点が飛ぶ。
void NavForwardToYawPitch(const math::Vector3& fwd, float& yaw, float& pitch)
{
    pitch = math::ToDeg(std::asin(math::Clamp(-fwd.y, -1.0f, 1.0f)));
    yaw   = math::ToDeg(std::atan2(fwd.x, fwd.z));
}

/// @brief ピボットと距離を保ったまま yaw / pitch を Teleport 要求へ流す。
/// @see PointEditorCamera
void NavApplyYawPitch(EditorContext& ctx, float yaw, float pitch)
{
    PointEditorCamera(ctx, yaw, pitch);
}

/// @return from → to の角度差を -180..180 に畳んだ値 (補間で遠回りさせない)。
float NavShortestAngle(float from, float to)
{
    float d = std::fmod(to - from + 540.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

} // namespace

bool IsOrientationGizmoHovered() { return g_navGizmo.hovered; }
bool IsOrientationGizmoActive()  { return g_navGizmo.pressed || g_navGizmo.animActive; }

void DrawOrientationGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    NavGizmoState& s = g_navGizmo;
    if (!ctx.editorCamera) {
        s = NavGizmoState{};
        return;
    }

    const ImVec2 center  = { viewportMin.x + viewportSize.x - kNavSize * 0.5f - kNavPadding,
                             viewportMin.y + kNavSize * 0.5f + kNavPadding };
    const float  radius  = kNavSize * 0.5f;
    const float  axisLen = radius - kNavBallNearR - 1.0f;

    /// @note view 行列の 3x3 は R^T なので、ワールド軸 e_i のビュー空間表現はその i 列目。ビュー空間は左手系で z がそのまま奥行き。
    const math::Matrix4 view = ctx.editorCamera->GetViewMatrix();

    struct NavBall {
        int    axis  = 0;
        ImVec2 pos{};
        float  depth = 0.0f;  ///< ビュー空間 z。大きいほど奥
    };
    NavBall balls[6];
    for (int i = 0; i < 6; ++i) {
        const math::Vector3& d = kNavAxes[i].dir;
        const math::Vector3  v = {
            view.m[0][0] * d.x + view.m[0][1] * d.y + view.m[0][2] * d.z,
            view.m[1][0] * d.x + view.m[1][1] * d.y + view.m[1][2] * d.z,
            view.m[2][0] * d.x + view.m[2][1] * d.y + view.m[2][2] * d.z,
        };
        balls[i].axis  = i;
        balls[i].pos   = { center.x + v.x * axisLen, center.y - v.y * axisLen };
        balls[i].depth = v.z;
    }

    int order[6] = { 0, 1, 2, 3, 4, 5 };
    std::sort(order, order + 6,
              [&](int a, int b) { return balls[a].depth > balls[b].depth; });

    const ImVec2 mouse = ImGui::GetMousePos();
    const float  mdx   = mouse.x - center.x;
    const float  mdy   = mouse.y - center.y;
    const bool   inRegion = ImGui::IsWindowHovered()
                         && (mdx * mdx + mdy * mdy) <= radius * radius;

    /// @note 重なったボールは手前を優先する (order の後ろほど手前)。
    int hoverAxis = -1;
    if (inRegion && !s.pressed) {
        constexpr float kHitR = kNavBallNearR + 2.0f;
        for (int k = 5; k >= 0; --k) {
            const NavBall& b  = balls[order[k]];
            const float    hx = mouse.x - b.pos.x;
            const float    hy = mouse.y - b.pos.y;
            if (hx * hx + hy * hy <= kHitR * kHitR) { hoverAxis = b.axis; break; }
        }
    }
    s.hovered     = inRegion;
    s.hoveredAxis = s.dragging ? -1 : (s.pressed ? s.pressedAxis : hoverAxis);

    /// @note Alt+左ドラッグは DebugCamera のオービットに割り当て済み。ここでも掴むと回転量が二重になる。
    if (inRegion && !ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s.pressed     = true;
        s.dragging    = false;
        s.pressedAxis = hoverAxis;
        s.pressPos    = mouse;
        s.animActive  = false;
        NavForwardToYawPitch(ctx.editorCamera->GetForward(), s.orbitYaw, s.orbitPitch);
    }

    if (s.pressed) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float mx = mouse.x - s.pressPos.x;
            const float my = mouse.y - s.pressPos.y;
            if (!s.dragging && (mx * mx + my * my) > kNavDragSlop * kNavDragSlop)
                s.dragging = true;
            if (s.dragging) {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                s.orbitYaw   += delta.x * kNavOrbitSens;
                s.orbitPitch += delta.y * kNavOrbitSens;
                s.orbitPitch  = math::Clamp(s.orbitPitch, -89.0f, 89.0f);
                NavApplyYawPitch(ctx, s.orbitYaw, s.orbitPitch);
            }
        } else {
            if (!s.dragging && s.pressedAxis >= 0) {
                const math::Vector3 targetFwd = kNavAxes[s.pressedAxis].dir * -1.0f;
                float toYaw = 0.0f, toPitch = 0.0f;
                NavForwardToYawPitch(targetFwd, toYaw, toPitch);
                NavForwardToYawPitch(ctx.editorCamera->GetForward(), s.fromYaw, s.fromPitch);
                /// @note 真上・真下は yaw が定まらないので今の向きを保つ。
                if (kNavAxes[s.pressedAxis].dir.y != 0.0f) toYaw = s.fromYaw;
                s.toYaw      = s.fromYaw + NavShortestAngle(s.fromYaw, toYaw);
                s.toPitch    = math::Clamp(toPitch, -89.9f, 89.9f);
                s.animT      = 0.0f;
                s.animActive = true;
            }
            s.pressed  = false;
            s.dragging = false;
        }
    }

    if (s.animActive) {
        s.animT += ImGui::GetIO().DeltaTime / kNavSnapTime;
        const float t = math::Clamp(s.animT, 0.0f, 1.0f);
        const float e = t * t * (3.0f - 2.0f * t);
        NavApplyYawPitch(ctx,
                         s.fromYaw   + (s.toYaw   - s.fromYaw)   * e,
                         s.fromPitch + (s.toPitch - s.fromPitch) * e);
        if (t >= 1.0f) s.animActive = false;
    }

    /// @note 背景円は描かない (シーンの見通しを塞ぐ)。代わりにボールと軸線に暗い縁取りを持たせる。
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int k = 0; k < 6; ++k) {
        const NavBall& b  = balls[order[k]];
        const NavAxis& ax = kNavAxes[b.axis];

        const float far01 = math::Clamp(b.depth * 0.5f + 0.5f, 0.0f, 1.0f);
        const float ballR = kNavBallNearR + (kNavBallFarR - kNavBallNearR) * far01;
        const float fade  = 1.0f - far01 * 0.45f;
        const bool  hot   = (s.hoveredAxis == b.axis);
        const ImU32 base  = kNavAxisColor[ax.colorIndex];

        if (ax.positive) {
            const float w = hot ? 3.0f : 2.2f;
            dl->AddLine(center, b.pos, IM_COL32(0, 0, 0, static_cast<int>(90 * fade)), w + 2.0f);
            dl->AddLine(center, b.pos, NavShade(base, hot ? 1.15f : 1.0f, 0.9f * fade), w);
        }

        if (ax.positive || hot) {
            dl->AddCircleFilled(b.pos, ballR, NavShade(base, hot ? 1.2f : 1.0f, fade), 20);
            dl->AddCircle(b.pos, ballR, NavShade(IM_COL32(0, 0, 0, 255), 1.0f, 0.47f * fade),
                          20, 1.5f);
        } else {
            dl->AddCircleFilled(b.pos, ballR, NavShade(base, 0.30f, 0.85f * fade), 20);
            dl->AddCircle(b.pos, ballR, NavShade(base, 1.0f, 0.90f * fade), 20, 1.8f);
        }

        /// @note 奥に回って縮んだボールは文字が潰れるのでラベルを出さない。
        if ((ax.positive || hot) && ballR >= 9.0f) {
            const ImVec2 ts = ImGui::CalcTextSize(ax.label);
            dl->AddText({ b.pos.x - ts.x * 0.5f, b.pos.y - ts.y * 0.5f },
                        NavShade(IM_COL32(18, 18, 20, 255), 1.0f, 0.92f * fade), ax.label);
        }

        if (hot)
            dl->AddCircle(b.pos, ballR + 3.0f, IM_COL32(255, 255, 255, 210), 24, 1.6f);
    }
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

    math::Matrix4 viewCol = ToColumnMajor(ctx.editorCamera->GetViewMatrix());
    math::Matrix4 projCol = ToColumnMajor(ctx.editorCamera->GetProjectionMatrix());

    /// @note Pivot はプライマリのワールド行列をそのまま使う。Center は選択バウンズ中心に置き、回転は Local のときだけプライマリのワールド回転を借りる。
    /// @note Center ではギズモ行列がどの Transform とも一致しないので、«ギズモの移動量» を全対象へ相対適用する経路に一本化する。
    const bool centerPivot = (ctx.gizmoPivot == EditorContext::GizmoPivot::Center);
    math::Matrix4 gizmoWorldRow = go->transform.GetWorldMatrix();
    if (centerPivot) {
        math::Vector3 center{};
        float         radius = 0.0f;
        if (ComputeSelectionBounds(ctx, center, radius)) {
            math::Matrix4 basis = math::Matrix4::Identity();
            if (ctx.gizmoSpace == EditorContext::GizmoSpace::Local)
                basis = math::Matrix4::Rotate(go->transform.worldRotation);
            basis.m[0][3] = center.x;
            basis.m[1][3] = center.y;
            basis.m[2][3] = center.z;
            gizmoWorldRow = basis;
        }
    }
    math::Matrix4 worldCol = ToColumnMajor(gizmoWorldRow);

    auto& style = ImGuizmo::GetStyle();
    style.Colors[ImGuizmo::SELECTION] = ImVec4(1.0f, 0.95f, 0.05f, 1.0f);
    style.Colors[ImGuizmo::INACTIVE] = ImVec4(0.75f, 0.75f, 0.75f, 0.75f);
    style.TranslationLineThickness = 4.0f;
    style.RotationLineThickness = 4.0f;
    style.ScaleLineThickness = 4.0f;

    ImGuizmo::SetDrawlist();
    ImGuizmo::Enable(true);
    /// @note ImGuizmo はハンドルの大きさを射影から逆算するので、ここを間違えると正投影でハンドルが伸び縮みして掴めない。
    ImGuizmo::SetOrthographic(
        ctx.editorCamera->m_projection == renderer::ProjectionMode::Orthographic);
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

    /// @note Unity と同じく Ctrl 押下中はモーメンタリスナップ。設定でスナップ ON なら常時。
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

    /// @brief マルチ選択ドラッグ 1 回分の編集状態。各配列は instanceIds と同じ並びで、[0] は必ずプライマリ。
    struct GizmoEdit {
        std::vector<std::string> instanceIds;
        std::vector<scene::Transform> before;
        std::vector<math::Matrix4> startWorld;
        std::vector<bool> applyDelta;  ///< true: ギズモの移動量を相対適用する対象
        math::Matrix4 gizmoStartWorldInv = math::Matrix4::Identity();  ///< delta = 現在のギズモ行列 * これ
        EditorContext::GizmoMode mode = EditorContext::GizmoMode::Translate;
        bool centerPivot = false;  ///< true ならプライマリにも delta を相対適用する (直接書き戻せない)
        bool active = false;
    };
    static GizmoEdit edit;

    if (gizmoUsing && !wasUsing) {
        edit.instanceIds.clear();
        edit.before.clear();
        edit.startWorld.clear();
        edit.applyDelta.clear();

        /// @note 子は親に追従するので、選択済みの祖先を持つ対象へ delta を掛けると二重に動く (Unity と同じ規則)。
        auto hasSelectedAncestor = [&ctx](scene::GameObject* obj) {
            for (scene::GameObject* p = obj->GetParent(); p; p = p->GetParent()) {
                if (std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(),
                              p->GetID()) != ctx.selectedEntities.end())
                    return true;
            }
            return false;
        };

        edit.instanceIds.push_back(go->instanceId);
        edit.before.push_back(go->transform);
        edit.startWorld.push_back(go->transform.GetWorldMatrix());
        edit.applyDelta.push_back(centerPivot && !hasSelectedAncestor(go));

        for (scene::EntityID id : ctx.selectedEntities) {
            if (id == selected) continue;
            scene::GameObject* sel = ctx.activeScene->GetGameObject(id);
            if (!sel || ctx.IsLocked(id)) continue;
            edit.instanceIds.push_back(sel->instanceId);
            edit.before.push_back(sel->transform);
            edit.startWorld.push_back(sel->transform.GetWorldMatrix());
            edit.applyDelta.push_back(!hasSelectedAncestor(sel));
        }

        edit.gizmoStartWorldInv = math::Matrix4::Inverse(gizmoWorldRow);
        edit.mode        = ctx.gizmoMode;
        edit.centerPivot = centerPivot;
        edit.active      = true;
    }

    if (gizmoOver != prevOver || gizmoUsing != prevUsing) {
        prevOver = gizmoOver;
        prevUsing = gizmoUsing;
    }

    if (!gizmoUsing) {
        if (wasUsing && edit.active) {
            /// @note 全対象の before/after を 1 コマンドにまとめる。分けるとマルチ選択の移動を選択数だけ Ctrl+Z する羽目になる。
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

    /// @note 添字 0 (プライマリ) より先に他へ適用する。選択中の親が動いた後にプライマリのワールド行列を書けば、プライマリはギズモ位置へ一致する。
    if (edit.active) {
        const math::Matrix4 delta = worldRow * edit.gizmoStartWorldInv;
        for (std::size_t i = edit.instanceIds.size(); i-- > 0; ) {
            if (!edit.applyDelta[i]) continue;
            if (auto* other = ctx.activeScene->FindByGuid(edit.instanceIds[i]))
                ApplyWorldRowToTransform(*other, delta * edit.startWorld[i]);
        }
    }

    /// @note Pivot のみ、ギズモ行列はプライマリのワールド行列そのものなので丸め誤差を挟まず直接書き戻す。
    if (!edit.centerPivot)
        ApplyWorldRowToTransform(*go, worldRow);
}

} // namespace fbzz::editor
