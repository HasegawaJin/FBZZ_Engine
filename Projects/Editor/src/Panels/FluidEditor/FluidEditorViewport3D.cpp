/// @file    FluidEditorViewport3D.cpp
/// @brief   Fluid Editor の 3D ライブプレビュー (共有 Baker のレイマーチ絵) と ImGuizmo による部品操作
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note 共有 Baker (VolumeFlipbookBaker) をそのまま使う: 3D 絵の経路はここだけで、160³ の GPU 資源を
/// @note       2 つ持つ余裕はない。FluidBakeService の 1 つを、焼きが使っていないフレームだけ借りる。
/// @note ギズモの座標系はレシピの正規化単位 [-1,1]³ (焼きの bake 空間と同じ) をそのまま使う。カメラは
/// @note       VolumeRaymarch.hlsl と同じ平行投影を組み直せば、絵とギズモが 1 画素もずれない。
#include "FluidEditorInternal.hpp"

#include <Editor/EditorContext.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/FluidVolumeBake.hpp>
#include <Engine/Asset/VolumeFlipbookBaker.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <ImGuizmo.h>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace fbzz::editor::fluideditor {
namespace {

/// @note 名前に 3D を入れるのは、Editor が Unity Build で同じ名前空間の無名 namespace が
/// @note       FluidEditorViewport.cpp と 1 つの翻訳単位に混ざり、定数名がぶつかると再定義になるため。
constexpr ImU32 kCanvas3DColor = IM_COL32(22, 22, 25, 255);
constexpr ImU32 kHint3DText = IM_COL32(200, 200, 205, 160);
constexpr ImU32 kBusy3DText = IM_COL32(255, 200, 80, 255);
constexpr ImU32 kWarn3DText = IM_COL32(255, 110, 90, 255);
constexpr ImU32 kAxisX3DColor = IM_COL32(238, 86, 100, 255);
constexpr ImU32 kAxisY3DColor = IM_COL32(150, 208, 72, 255);
constexpr ImU32 kAxisZ3DColor = IM_COL32(74, 144, 236, 255);
constexpr float kMinGizmoSize = 0.01f;
/// @note レイの出発点 (VolumeRaymarch.hlsl の gCamForward * 4)。箱 [-1,1]³ は前後とも余裕を持って入る。
constexpr float kEyeDistance = 4.0f;
constexpr float kNav3DSize = 104.0f;
constexpr float kNav3DPadding = 10.0f;
constexpr float kNav3DBallNearRadius = 11.0f;
constexpr float kNav3DBallFarRadius = 7.5f;
constexpr float kNav3DSnapTime = 0.26f;
constexpr float kNav3DDragSlop = 4.0f;
constexpr float kNav3DOrbitSensitivity = 0.45f;
constexpr float kDegreesToRadians3D = 0.017453292519943295f;
/// @note プレビュー RT は [色 | 速度 | 6-way 正 | 6-way 負 | Albedo 色 | Emission 色] が横並び。先頭タイルだけ表示する。
constexpr float kColorTileU1 = 1.0f / 6.0f;

struct VolumeGizmoCamera {
    math::Matrix4 viewRow;
    math::Matrix4 projRow;
    math::Matrix4 viewProjRow;
};

struct VolumeNavAxis {
    math::Vector3 direction;
    int colorIndex;
    const char* label;
    bool positive;
};

struct VolumeNavBall {
    int axis = 0;
    ImVec2 position{};
    float depth = 0.0f;
};

const VolumeNavAxis kVolumeNavAxes[6] = {
    { { 1.0f, 0.0f, 0.0f }, 0, "X", true },
    { { -1.0f, 0.0f, 0.0f }, 0, "X", false },
    { { 0.0f, 1.0f, 0.0f }, 1, "Y", true },
    { { 0.0f, -1.0f, 0.0f }, 1, "Y", false },
    { { 0.0f, 0.0f, 1.0f }, 2, "Z", true },
    { { 0.0f, 0.0f, -1.0f }, 2, "Z", false },
};

struct GizmoPartPose {
    bool valid = false;
    /// @note 画面に出ている位置 (center + 動きのずれ。キーを選んでいればそのキーの位置)。
    math::Vector3 position;
    math::Vector3 size{ 1.0f, 1.0f, 1.0f };
    math::Vector3 direction{ 0.0f, 1.0f, 0.0f };
    bool canScale = false;
    bool canRotate = false;
};

/// @note 画面に出す一辺 (画素) から、プレビューのタイル解像度を決める。
/// @note 画面に合わせるのは、表示サイズよりタイルが小さいと拡大されてテクセルが四角く見えるため
/// @note       (frame_size は «焼くときのコマの大きさ» で表示画素数とは無関係)。64 画素刻みに丸めるのは、
/// @note       この値がプレビューの鍵に入っており、1 画素変わるたび鍵が変わって解き直しになるため。
int PreviewTileSize(float viewSide, int cap)
{
    constexpr int kStep = 64;
    /// @note まだ一度も描いていない
    if (viewSide <= 0.0f) return cap;
    const int wanted = static_cast<int>(std::ceil(viewSide / static_cast<float>(kStep))) * kStep;
    return std::clamp(wanted, 128, cap);
}

asset::VolumeFlipbookBakeSettings VolumePreviewSettings(const State& state)
{
    asset::VolumeFlipbookBakeSettings settings =
        asset::MakeVolumeBakeSettings(state.document.Recipe(), state.document.Path());
    /// @note 焼きと同じ重さで毎フレーム解くとエディターごと止まる。2D と同じ Draft / Normal / Final で落とす。
    switch (state.preview.Quality()) {
    case FluidPreviewQuality::Draft:
        settings.volumeResolution = (std::min)(settings.volumeResolution, 48);
        settings.tileSize = PreviewTileSize(state.volumeViewSide, 256);
        settings.raySteps = (std::min)(settings.raySteps, 48);
        settings.shadowSteps = (std::min)(settings.shadowSteps, 6);
        break;
    case FluidPreviewQuality::Normal:
        settings.volumeResolution = (std::min)(settings.volumeResolution, 96);
        settings.tileSize = PreviewTileSize(state.volumeViewSide, 384);
        settings.raySteps = (std::min)(settings.raySteps, 96);
        settings.shadowSteps = (std::min)(settings.shadowSteps, 12);
        break;
    case FluidPreviewQuality::Final:
        /// @note プレビューだけ 128 で止めるのは、CPU ソルバーの上限が 96 (kMaxFluidResolution) で、
        /// @note       素通しにすると «切り替えただけ» でセル数が 4.6 倍に跳ね、追いつきの Dispatch が
        /// @note       GPU のウォッチドッグに掛かるため。160 を使ってよいのは焼き (Begin) だけ。
        settings.volumeResolution = (std::min)(settings.volumeResolution, 128);
        settings.tileSize = PreviewTileSize(state.volumeViewSide, 512);
        break;
    }
    /// @note 見るのは色のタイル 1 枚。6-way と supersampling は見えない所に時間を使うだけ。
    settings.sixWayLightmaps = false;
    settings.supersampling = 1;
    return settings;
}

asset::VolumeFlipbookCamera MakeVolumePreviewBasis(const asset::VolumeFlipbookBakeSettings& settings,
                                                   float yawDegrees, float pitchDegrees)
{
    asset::VolumeFlipbookCamera camera = asset::ComputeVolumeFlipbookCamera(settings);
    const float yaw = yawDegrees * kDegreesToRadians3D;
    const float pitch = std::clamp(pitchDegrees, -89.0f, 89.0f) * kDegreesToRadians3D;
    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);
    const float cosPitch = std::cos(pitch);
    const float sinPitch = std::sin(pitch);
    camera.forward = { sinYaw * cosPitch, -sinPitch, cosYaw * cosPitch };
    camera.right = { cosYaw, 0.0f, -sinYaw };
    camera.up = math::Vector3::Cross(camera.forward, camera.right).NormalizedOr(math::Vector3::UP);
    return camera;
}

VolumeGizmoCamera MakeVolumePreviewCamera(const asset::VolumeFlipbookBakeSettings& settings,
                                          const asset::VolumeFlipbookCamera& camera)
{
    const float halfExtent = (std::max)(settings.halfExtent, 0.05f);
    const math::Vector3 eye = camera.forward * -kEyeDistance;
    VolumeGizmoCamera out;
    out.viewRow = math::Matrix4::LookAt(eye, eye + camera.forward, camera.up);
    out.projRow = math::Matrix4::Orthographic(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.01f,
                                              kEyeDistance * 2.0f);
    out.viewProjRow = out.projRow * out.viewRow;
    return out;
}

void InitializeVolumeOrbit(State& state, const asset::VolumeFlipbookBakeSettings& settings)
{
    const std::string& path = state.document.Path();
    if (state.volumeOrbit.initialized && state.volumeOrbit.recipePath == path) return;
    state.volumeOrbit = {};
    state.volumeOrbit.recipePath = path;
    state.volumeOrbit.yawDegrees = settings.cameraYawDegrees;
    state.volumeOrbit.initialized = true;
}

void VolumeForwardToYawPitch(const math::Vector3& forward, float& yaw, float& pitch)
{
    pitch = math::ToDeg(std::asin(math::Clamp(-forward.y, -1.0f, 1.0f)));
    yaw = math::ToDeg(std::atan2(forward.x, forward.z));
}

float VolumeShortestAngle(float from, float to)
{
    float difference = std::fmod(to - from + 540.0f, 360.0f);
    if (difference < 0.0f) difference += 360.0f;
    return difference - 180.0f;
}

void StartVolumeOrbitSnap(VolumeOrbitState& orbit, float yawDegrees, float pitchDegrees)
{
    orbit.fromYaw = orbit.yawDegrees;
    orbit.fromPitch = orbit.pitchDegrees;
    orbit.toYaw = orbit.yawDegrees + VolumeShortestAngle(orbit.yawDegrees, yawDegrees);
    orbit.toPitch = std::clamp(pitchDegrees, -89.9f, 89.9f);
    orbit.animT = 0.0f;
    orbit.animActive = true;
}

bool ProjectToPreviewSquare(const math::Matrix4& viewProjRow, const math::Vector3& point, ImVec2 min, float side,
                            ImVec2& out)
{
    const math::Vector4 clip = viewProjRow * math::Vector4{ point.x, point.y, point.z, 1.0f };
    if (std::fabs(clip.w) < 1.0e-6f) return false;
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    out = { min.x + (ndcX * 0.5f + 0.5f) * side, min.y + (0.5f - ndcY * 0.5f) * side };
    return true;
}

void DrawVolumeSpatialLine(ImDrawList* drawList, const VolumeGizmoCamera& camera, ImVec2 min, float side,
                           const math::Vector3& from, const math::Vector3& to, ImU32 color, float thickness)
{
    ImVec2 a;
    ImVec2 b;
    if (ProjectToPreviewSquare(camera.viewProjRow, from, min, side, a)
        && ProjectToPreviewSquare(camera.viewProjRow, to, min, side, b))
        drawList->AddLine(a, b, color, thickness);
}

void DrawVolumeSpatialGuides(ImDrawList* drawList, const VolumeGizmoCamera& camera, ImVec2 min, float side)
{
    constexpr ImU32 kBounds3DColor = IM_COL32(220, 228, 238, 86);
    constexpr ImU32 kGrid3DColor = IM_COL32(160, 174, 194, 36);
    constexpr ImU32 kAxis3DColor[3] = {
        IM_COL32(238, 86, 100, 116), IM_COL32(150, 208, 72, 116), IM_COL32(74, 144, 236, 116),
    };

    constexpr math::Vector3 corners[8] = {
        { -1.0f, -1.0f, -1.0f }, { 1.0f, -1.0f, -1.0f },
        { -1.0f, 1.0f, -1.0f },  { 1.0f, 1.0f, -1.0f },
        { -1.0f, -1.0f, 1.0f },  { 1.0f, -1.0f, 1.0f },
        { -1.0f, 1.0f, 1.0f },   { 1.0f, 1.0f, 1.0f },
    };
    constexpr int edges[12][2] = {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };
    for (const auto& edge : edges)
        DrawVolumeSpatialLine(drawList, camera, min, side, corners[edge[0]], corners[edge[1]],
                              kBounds3DColor, 1.0f);

    for (int step = -2; step <= 2; ++step) {
        const float coordinate = static_cast<float>(step) * 0.5f;
        DrawVolumeSpatialLine(drawList, camera, min, side, { -1.0f, -1.0f, coordinate },
                              { 1.0f, -1.0f, coordinate }, kGrid3DColor, 1.0f);
        DrawVolumeSpatialLine(drawList, camera, min, side, { coordinate, -1.0f, -1.0f },
                              { coordinate, -1.0f, 1.0f }, kGrid3DColor, 1.0f);
    }

    const math::Vector3 origin{};
    const math::Vector3 kAxisEnds[3] = {
        { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    };
    for (int axis = 0; axis < 3; ++axis) {
        DrawVolumeSpatialLine(drawList, camera, min, side, origin, kAxisEnds[axis], kAxis3DColor[axis], 1.5f);
        ImVec2 label;
        if (ProjectToPreviewSquare(camera.viewProjRow, kAxisEnds[axis] * 1.08f, min, side, label)) {
            const char* axisLabel = axis == 0 ? "X" : axis == 1 ? "Y" : "Z";
            drawList->AddText(label, kAxis3DColor[axis], axisLabel);
        }
    }

    ImVec2 center;
    if (ProjectToPreviewSquare(camera.viewProjRow, origin, min, side, center)) {
        drawList->AddCircleFilled(center, 2.0f, IM_COL32(245, 245, 250, 150), 12);
        drawList->AddCircle(center, 4.0f, IM_COL32(0, 0, 0, 120), 16, 1.0f);
    }
}

const char* VolumeCenterPlaneLabel(VolumeCenterPlane plane)
{
    switch (plane) {
    case VolumeCenterPlane::XY: return "XY (Z=0)";
    case VolumeCenterPlane::XZ: return "XZ (Y=0)";
    case VolumeCenterPlane::YZ: return "YZ (X=0)";
    }
    return "XY (Z=0)";
}

void DrawVolumeCenterPlaneGuide(ImDrawList* drawList, const VolumeGizmoCamera& camera, ImVec2 min, float side,
                                VolumeCenterPlane plane)
{
    constexpr ImU32 kReferenceGrid3DColor = IM_COL32(180, 194, 214, 17);
    constexpr ImU32 kReferenceAxis3DColors[3] = {
        IM_COL32(238, 86, 100, 43), IM_COL32(150, 208, 72, 43), IM_COL32(74, 144, 236, 43),
    };

    int uAxis = 0;
    int vAxis = 1;
    if (plane == VolumeCenterPlane::XZ) {
        vAxis = 2;
    } else if (plane == VolumeCenterPlane::YZ) {
        uAxis = 1;
        vAxis = 2;
    }
    const auto pointOnPlane = [plane](float u, float v) {
        switch (plane) {
        case VolumeCenterPlane::XY: return math::Vector3{ u, v, 0.0f };
        case VolumeCenterPlane::XZ: return math::Vector3{ u, 0.0f, v };
        case VolumeCenterPlane::YZ: return math::Vector3{ 0.0f, u, v };
        }
        return math::Vector3{ u, v, 0.0f };
    };

    for (int step = -2; step <= 2; ++step) {
        const float coordinate = static_cast<float>(step) * 0.5f;
        const ImU32 acrossU = step == 0 ? kReferenceAxis3DColors[uAxis] : kReferenceGrid3DColor;
        const ImU32 acrossV = step == 0 ? kReferenceAxis3DColors[vAxis] : kReferenceGrid3DColor;
        DrawVolumeSpatialLine(drawList, camera, min, side, pointOnPlane(-1.0f, coordinate),
                              pointOnPlane(1.0f, coordinate), acrossU, 1.0f);
        DrawVolumeSpatialLine(drawList, camera, min, side, pointOnPlane(coordinate, -1.0f),
                              pointOnPlane(coordinate, 1.0f), acrossV, 1.0f);
    }
}

ImU32 ShadeVolumeNavColor(ImU32 color, float brightness, float alpha)
{
    const auto channel = [&](int shift) {
        const float value = static_cast<float>((color >> shift) & 0xFFu) * brightness;
        return static_cast<ImU32>(std::clamp(value, 0.0f, 255.0f));
    };
    return IM_COL32(channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT),
                    static_cast<ImU32>(std::clamp(alpha * 255.0f, 0.0f, 255.0f)));
}

ImU32 VolumeNavAxisColor(int index)
{
    constexpr ImU32 colors[3] = { kAxisX3DColor, kAxisY3DColor, kAxisZ3DColor };
    return colors[std::clamp(index, 0, 2)];
}

ImVec2 VolumeNavCenter(const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    const float radius = kNav3DSize * 0.5f;
    return { viewportMin.x + viewportSize.x - radius - kNav3DPadding,
             viewportMin.y + radius + kNav3DPadding };
}

void BuildVolumeNavBalls(const VolumeGizmoCamera& camera, const ImVec2& center, VolumeNavBall (&balls)[6])
{
    const float axisLength = kNav3DSize * 0.5f - kNav3DBallNearRadius - 1.0f;
    for (int i = 0; i < 6; ++i) {
        const math::Vector3& direction = kVolumeNavAxes[i].direction;
        const math::Vector3 viewDirection{
            camera.viewRow.m[0][0] * direction.x + camera.viewRow.m[0][1] * direction.y
                + camera.viewRow.m[0][2] * direction.z,
            camera.viewRow.m[1][0] * direction.x + camera.viewRow.m[1][1] * direction.y
                + camera.viewRow.m[1][2] * direction.z,
            camera.viewRow.m[2][0] * direction.x + camera.viewRow.m[2][1] * direction.y
                + camera.viewRow.m[2][2] * direction.z,
        };
        balls[i].axis = i;
        balls[i].position = { center.x + viewDirection.x * axisLength,
                              center.y - viewDirection.y * axisLength };
        balls[i].depth = viewDirection.z;
    }
}

bool UpdateVolumeOrientationGizmo(State& state, const asset::VolumeFlipbookBakeSettings& settings,
                                  const ImVec2& viewportMin, const ImVec2& viewportSize,
                                  bool viewportHovered)
{
    VolumeOrbitState& orbit = state.volumeOrbit;
    const float availableSide = (std::min)(viewportSize.x, viewportSize.y);
    if (availableSide < 80.0f && !orbit.pressed && !orbit.animActive) {
        orbit.hovered = false;
        orbit.hoveredAxis = -1;
        return false;
    }

    const ImVec2 center = VolumeNavCenter(viewportMin, viewportSize);
    const float radius = kNav3DSize * 0.5f;
    const asset::VolumeFlipbookCamera basis =
        MakeVolumePreviewBasis(settings, orbit.yawDegrees, orbit.pitchDegrees);
    const VolumeGizmoCamera camera = MakeVolumePreviewCamera(settings, basis);
    VolumeNavBall balls[6];
    BuildVolumeNavBalls(camera, center, balls);

    int order[6] = { 0, 1, 2, 3, 4, 5 };
    std::sort(order, order + 6, [&](int a, int b) { return balls[a].depth > balls[b].depth; });
    const ImVec2 mouse = ImGui::GetMousePos();
    const float dx = mouse.x - center.x;
    const float dy = mouse.y - center.y;
    const bool inRegion = viewportHovered && !state.gizmoActive && dx * dx + dy * dy <= radius * radius;
    int hoverAxis = -1;
    if (inRegion && !orbit.pressed) {
        constexpr float hitRadius = kNav3DBallNearRadius + 2.0f;
        for (int k = 5; k >= 0; --k) {
            const VolumeNavBall& ball = balls[order[k]];
            const float hx = mouse.x - ball.position.x;
            const float hy = mouse.y - ball.position.y;
            if (hx * hx + hy * hy <= hitRadius * hitRadius) {
                hoverAxis = ball.axis;
                break;
            }
        }
    }
    orbit.hovered = inRegion;
    orbit.hoveredAxis = orbit.dragging ? -1 : (orbit.pressed ? orbit.pressedAxis : hoverAxis);

    const ImGuiIO& io = ImGui::GetIO();
    if (inRegion && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        orbit.pressed = true;
        orbit.dragging = false;
        orbit.pressedAxis = hoverAxis;
        orbit.pressPos = mouse;
        orbit.animActive = false;
    }

    if (orbit.pressed) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float pressX = mouse.x - orbit.pressPos.x;
            const float pressY = mouse.y - orbit.pressPos.y;
            if (!orbit.dragging && pressX * pressX + pressY * pressY > kNav3DDragSlop * kNav3DDragSlop)
                orbit.dragging = true;
            if (orbit.dragging) {
                orbit.yawDegrees += io.MouseDelta.x * kNav3DOrbitSensitivity;
                orbit.pitchDegrees = std::clamp(orbit.pitchDegrees + io.MouseDelta.y * kNav3DOrbitSensitivity,
                                                -89.0f, 89.0f);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        } else {
            if (!orbit.dragging && orbit.pressedAxis >= 0) {
                const math::Vector3 targetForward = kVolumeNavAxes[orbit.pressedAxis].direction * -1.0f;
                float targetYaw = 0.0f;
                float targetPitch = 0.0f;
                VolumeForwardToYawPitch(targetForward, targetYaw, targetPitch);
                if (kVolumeNavAxes[orbit.pressedAxis].direction.y != 0.0f) targetYaw = orbit.yawDegrees;
                StartVolumeOrbitSnap(orbit, targetYaw, targetPitch);
            }
            orbit.pressed = false;
            orbit.dragging = false;
        }
    }

    if (orbit.animActive) {
        orbit.animT += io.DeltaTime / kNav3DSnapTime;
        const float t = std::clamp(orbit.animT, 0.0f, 1.0f);
        const float eased = t * t * (3.0f - 2.0f * t);
        orbit.yawDegrees = orbit.fromYaw + (orbit.toYaw - orbit.fromYaw) * eased;
        orbit.pitchDegrees = orbit.fromPitch + (orbit.toPitch - orbit.fromPitch) * eased;
        if (t >= 1.0f) orbit.animActive = false;
    }
    return orbit.hovered || orbit.pressed || orbit.animActive;
}

void DrawVolumeOrientationGizmo(ImDrawList* drawList, State& state,
                                const VolumeGizmoCamera& camera, const ImVec2& viewportMin,
                                const ImVec2& viewportSize)
{
    if ((std::min)(viewportSize.x, viewportSize.y) < 80.0f) return;
    const VolumeOrbitState& orbit = state.volumeOrbit;
    const ImVec2 center = VolumeNavCenter(viewportMin, viewportSize);
    VolumeNavBall balls[6];
    BuildVolumeNavBalls(camera, center, balls);
    int order[6] = { 0, 1, 2, 3, 4, 5 };
    std::sort(order, order + 6, [&](int a, int b) { return balls[a].depth > balls[b].depth; });

    for (int k = 0; k < 6; ++k) {
        const VolumeNavBall& ball = balls[order[k]];
        const VolumeNavAxis& axis = kVolumeNavAxes[ball.axis];
        const float far01 = std::clamp(ball.depth * 0.5f + 0.5f, 0.0f, 1.0f);
        const float ballRadius = kNav3DBallNearRadius + (kNav3DBallFarRadius - kNav3DBallNearRadius) * far01;
        const float fade = 1.0f - far01 * 0.45f;
        const bool hot = orbit.hoveredAxis == ball.axis;
        const ImU32 base = VolumeNavAxisColor(axis.colorIndex);

        if (axis.positive) {
            const float width = hot ? 3.0f : 2.2f;
            drawList->AddLine(center, ball.position, IM_COL32(0, 0, 0, static_cast<int>(90 * fade)), width + 2.0f);
            drawList->AddLine(center, ball.position,
                              ShadeVolumeNavColor(base, hot ? 1.15f : 1.0f, 0.9f * fade), width);
        }
        if (axis.positive || hot) {
            drawList->AddCircleFilled(ball.position, ballRadius,
                                      ShadeVolumeNavColor(base, hot ? 1.2f : 1.0f, fade), 20);
            drawList->AddCircle(ball.position, ballRadius,
                                ShadeVolumeNavColor(IM_COL32(0, 0, 0, 255), 1.0f, 0.47f * fade), 20, 1.5f);
        } else {
            drawList->AddCircleFilled(ball.position, ballRadius, ShadeVolumeNavColor(base, 0.30f, 0.85f * fade), 20);
            drawList->AddCircle(ball.position, ballRadius, ShadeVolumeNavColor(base, 1.0f, 0.90f * fade), 20, 1.8f);
        }
        if ((axis.positive || hot) && ballRadius >= 9.0f) {
            const ImVec2 textSize = ImGui::CalcTextSize(axis.label);
            drawList->AddText({ ball.position.x - textSize.x * 0.5f, ball.position.y - textSize.y * 0.5f },
                              ShadeVolumeNavColor(IM_COL32(18, 18, 20, 255), 1.0f, 0.92f * fade), axis.label);
        }
        if (hot) drawList->AddCircle(ball.position, ballRadius + 3.0f, IM_COL32(255, 255, 255, 210), 24, 1.6f);
    }
}

/// @note +Y を direction に合わせた正規直交基底 (回転ギズモが掴む向き)。
math::Matrix4 GizmoBasisFromDirection(const math::Vector3& direction)
{
    const math::Vector3 y = direction.NormalizedOr(math::Vector3::UP);
    const math::Vector3 reference = std::fabs(y.y) > 0.99f ? math::Vector3::RIGHT : math::Vector3::UP;
    const math::Vector3 x = math::Vector3::Cross(reference, y).NormalizedOr(math::Vector3::RIGHT);
    const math::Vector3 z = math::Vector3::Cross(x, y);
    math::Matrix4 basis = math::Matrix4::Identity();
    basis.m[0][0] = x.x; basis.m[1][0] = x.y; basis.m[2][0] = x.z;
    basis.m[0][1] = y.x; basis.m[1][1] = y.y; basis.m[2][1] = y.z;
    basis.m[0][2] = z.x; basis.m[1][2] = z.y; basis.m[2][2] = z.z;
    return basis;
}

math::Vector3 ExtractMatrixScale(const math::Matrix4& row)
{
    const auto axis = [&row](int c) {
        return std::sqrt(row.m[0][c] * row.m[0][c] + row.m[1][c] * row.m[1][c] + row.m[2][c] * row.m[2][c]);
    };
    return { axis(0), axis(1), axis(2) };
}

GizmoPartPose ReadGizmoPose(const fluid::FluidRecipe& recipe, const FluidSelection& selection, int keyIndex,
                            float solverTime)
{
    GizmoPartPose pose;
    VisitPart(recipe, selection.kind, selection.index, [&](const auto& part) {
        using Part = std::decay_t<decltype(part)>;
        pose.valid = true;
        if (keyIndex >= 0 && keyIndex < static_cast<int>(part.motion.keys.size()))
            pose.position = part.center + part.motion.keys[static_cast<std::size_t>(keyIndex)].offset;
        else
            pose.position = part.center + MotionOffsetAt(part.motion, solverTime);
        pose.direction = part.direction;
        if constexpr (std::is_same_v<Part, fluid::FluidForce>) {
            /// @note 半径 0 は «領域全体に一様» なので、掴める大きさが無い。
            pose.canScale = part.radius > 0.0f;
            const float radius = (std::max)(part.radius, kMinGizmoSize);
            pose.size = { radius, radius, radius };
            pose.canRotate = part.type == fluid::FluidForceType::Wind || part.type == fluid::FluidForceType::Vortex;
        } else if constexpr (std::is_same_v<Part, fluid::FluidCollider>) {
            pose.size = part.size;
            pose.canScale = part.shape != fluid::FluidColliderShape::Plane;
            /// @note 平面は法線、カプセルと円柱は芯の軸を direction で持つ。
            pose.canRotate = part.shape == fluid::FluidColliderShape::Plane
                          || part.shape == fluid::FluidColliderShape::Capsule
                          || part.shape == fluid::FluidColliderShape::Cylinder;
        } else {
            pose.size = part.size;
            pose.canScale = true;
            pose.canRotate = part.shape != fluid::FluidSourceShape::Sphere
                          && part.shape != fluid::FluidSourceShape::Box;
        }
    });
    return pose;
}

void ApplyGizmoEdit(fluid::FluidRecipe& recipe, const FluidSelection& selection, int keyIndex, GizmoOp op,
                    const math::Matrix4& worldRow, float solverTime)
{
    VisitPart(recipe, selection.kind, selection.index, [&](auto& part) {
        using Part = std::decay_t<decltype(part)>;
        if (op == GizmoOp::Translate) {
            const math::Vector3 target{ worldRow.m[0][3], worldRow.m[1][3], worldRow.m[2][3] };
            if (keyIndex >= 0 && keyIndex < static_cast<int>(part.motion.keys.size()))
                part.motion.keys[static_cast<std::size_t>(keyIndex)].offset = target - part.center;
            else
                part.center = target - MotionOffsetAt(part.motion, solverTime);
            return;
        }
        if (op == GizmoOp::Scale) {
            const math::Vector3 scale = ExtractMatrixScale(worldRow);
            if constexpr (std::is_same_v<Part, fluid::FluidForce>) {
                part.radius = (std::max)((scale.x + scale.y + scale.z) / 3.0f, kMinGizmoSize);
            } else {
                math::Vector3 next{ (std::max)(scale.x, kMinGizmoSize), (std::max)(scale.y, kMinGizmoSize),
                                    (std::max)(scale.z, kMinGizmoSize) };
                /// @note 球は size.x しか読まれない。3 軸がばらけると «絵は変わらないのに数字だけ動く» になる。
                bool uniform = false;
                if constexpr (std::is_same_v<Part, fluid::FluidCollider>)
                    uniform = part.shape == fluid::FluidColliderShape::Sphere;
                else
                    uniform = part.shape == fluid::FluidSourceShape::Sphere;
                if (uniform) {
                    const float side = (next.x + next.y + next.z) / 3.0f;
                    next = { side, side, side };
                }
                part.size = next;
            }
            return;
        }
        const math::Vector3 axis{ worldRow.m[0][1], worldRow.m[1][1], worldRow.m[2][1] };
        const float length = part.direction.Length();
        const math::Vector3 unit = axis.NormalizedOr(math::Vector3::UP);
        part.direction = length > 1.0e-4f ? unit * length : unit;
    });
}

const char* GizmoUndoLabel(GizmoOp op)
{
    switch (op) {
    case GizmoOp::Translate: return "Move Fluid Part";
    case GizmoOp::Rotate:    return "Rotate Fluid Part";
    case GizmoOp::Scale:     return "Resize Fluid Part";
    }
    return "Move Fluid Part";
}

/// @note 部品の中心を小さな丸で示し、一番近いものを返す (クリックで選ぶ)。
FluidSelection DrawVolumePartMarkers(ImDrawList* drawList, const State& state, const VolumeGizmoCamera& camera,
                                     ImVec2 min, float side, ImVec2 mouse, float solverTime, bool allowPick)
{
    constexpr FluidSelectionKind kLists[] = { FluidSelectionKind::Source, FluidSelectionKind::Force,
                                              FluidSelectionKind::Collider };
    constexpr float kPickRadius = 10.0f;
    const FluidDocument& document = state.document;
    const fluid::FluidRecipe& recipe = document.Recipe();
    FluidSelection nearest;
    float nearestDistance = kPickRadius;

    for (const FluidSelectionKind list : kLists) {
        for (int index = 0; index < MaxParts(list); ++index) {
            const GizmoPartPose pose = ReadGizmoPose(recipe, FluidSelection{ list, index }, -1, solverTime);
            if (!pose.valid) break;
            if (!PartShownInPreview(document, list, index)) continue;
            ImVec2 screen;
            if (!ProjectToPreviewSquare(camera.viewProjRow, pose.position, min, side, screen)) continue;
            const bool selected = document.selection.kind == list && document.selection.index == index;
            drawList->AddCircleFilled(screen, selected ? 4.0f : 3.0f, ListColor(list, selected ? 1.0f : 0.7f));
            drawList->AddCircle(screen, selected ? 6.0f : 4.5f, IM_COL32(0, 0, 0, 160));
            if (!allowPick) continue;
            const float distance = (std::max)(std::fabs(mouse.x - screen.x), std::fabs(mouse.y - screen.y));
            if (distance <= nearestDistance) {
                nearestDistance = distance;
                nearest = FluidSelection{ list, index };
            }
        }
    }
    return nearest;
}

}

void TickVolumePreview(EditorContext& ctx, State& state)
{
    state.volumeBusy = false;
    state.volumePending = false;
    state.volumeSwitchPending = false;
    state.volumeStale = false;
    if (!state.document.IsOpen()) return;
    /// @note 描く前にここで決めておくと、3D で焼くレシピは開いた 1 コマ目から 3D の絵が出る。
    if (!state.viewModeChosen) {
        state.viewMode = state.document.Recipe().bake.mode == fluid::FluidBakeMode::Volume3D
            ? ViewportMode::Volume3D
            : ViewportMode::Flat2D;
        state.viewModeChosen = true;
    }
    if (state.viewMode != ViewportMode::Volume3D) return;
    const asset::VolumeFlipbookBakeSettings settings = VolumePreviewSettings(state);
    InitializeVolumeOrbit(state, settings);
    if (ctx.fluidBake == nullptr || ctx.renderer == nullptr || ctx.resources == nullptr) return;

    FluidBakeService& service = *ctx.fluidBake;
    if (!service.IsVolumeBakerFree()) {
        state.volumeBusy = true;
        return;
    }

    /// @note hide / solo は文書の Revision を進めない。2D と同じく表示の通番を混ぜた鍵で解き直しを決める。
    /// @note       +1 は «まだ組んでいない» の 0 と、版数 0・通番 0 の初回を分けるため。
    const std::uint64_t key = state.document.Revision() * 1000003ull + state.visibilityGeneration + 1ull;
    if (key != state.volumeRecipeKey) {
        state.volumeRecipe = state.document.PreviewRecipe();
        state.volumeRecipeKey = key;
    }

    /// @note ソルバーが入れ替わると絵の中身も入れ替わる。再生の途中でそれをやると «同じ一続きの動き» が
    /// @note       途中で別物にすり替わるので、折り返し (playhead が戻るフレーム) まで前のソルバーで通す。
    /// @note       止まっているときは待つ理由が無い ── その場で切り替える。
    const bool wrapped = state.playhead < state.volumeSwitchPlayhead;
    /// @note 期限を切るのは、loop = false のレシピは折り返しが永遠に来ないため (テンプレートの半分がそれ)。
    /// @note       «折り返しまで待つ» だけだと再生中の切り替えでゲートが開かず絵が出ないままになる。
    /// @note       待つのは続きの動きを守るためなので、少し待って来なければ諦めて当てる。
    if (service.VolumePreviewSwitchPending())
        state.volumeSwitchWait += ImGui::GetIO().DeltaTime;
    else
        state.volumeSwitchWait = 0.0f;
    constexpr float kSwitchWaitLimit = 0.5f;
    const bool waitedEnough = state.volumeSwitchWait >= kSwitchWaitLimit;
    service.AllowVolumePreviewSwitch(!state.playing || wrapped || waitedEnough);
    if (wrapped || waitedEnough) state.volumeSwitchWait = 0.0f;
    state.volumeSwitchPlayhead = state.playhead;

    asset::VolumePreviewOptions options;
    options.view = asset::VolumePreviewView::Color;
    options.background = state.checkerBackground ? asset::VolumePreviewBackground::Checker
                                                 : asset::VolumePreviewBackground::Dark;
    options.cameraOverride = MakeVolumePreviewBasis(settings, state.volumeOrbit.yawDegrees,
                                                    state.volumeOrbit.pitchDegrees);
    (void)service.RecordVolumePreview(ctx, settings, state.playhead, options, &state.volumeRecipe, key);
    state.volumePending = service.VolumeBaker().PreviewPending();
    state.volumeSwitchPending = service.VolumePreviewSwitchPending();
    state.volumeStale = service.VolumePreviewStale();
}

void DrawViewport3D(EditorContext& ctx, State& state)
{
    FluidDocument& document = state.document;
    const fluid::FluidRecipe& recipe = document.Recipe();
    const ImGuiIO& io = ImGui::GetIO();
    const int activeKey = ActiveMotionKey(state);
    const float solverTime = recipe.output.warmup + state.playhead;
    const asset::VolumeFlipbookBakeSettings settings = VolumePreviewSettings(state);
    InitializeVolumeOrbit(state, settings);
    const GizmoPartPose pose = document.selection.IsPart()
        ? ReadGizmoPose(recipe, document.selection, activeKey, solverTime)
        : GizmoPartPose{};
    const bool rotateSupported = !pose.valid || (pose.canRotate && activeKey < 0);
    const bool scaleSupported = !pose.valid || (pose.canScale && activeKey < 0);

    const auto gizmoButton = [&state](const char* label, GizmoOp op, bool supported, const char* tip) {
        ImGui::BeginDisabled(!supported);
        ImGui::PushStyleColor(ImGuiCol_Button,
            state.gizmoOp == op
                ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        const bool pressed = ImGui::SmallButton(label);
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
        return pressed;
    };
    if (gizmoButton("Move##fe3d_move", GizmoOp::Translate, true,
                    "Move: 部品の中心または選択中のキーを動かす"))
        state.gizmoOp = GizmoOp::Translate;
    if (gizmoButton("Rot##fe3d_rotate", GizmoOp::Rotate, rotateSupported,
                    rotateSupported ? "Rotate: 対応形状の向きを変える" : "この形状または選択中のキーでは回転できません"))
        state.gizmoOp = GizmoOp::Rotate;
    if (gizmoButton("Scale##fe3d_scale", GizmoOp::Scale, scaleSupported,
                    scaleSupported ? "Scale: 対応形状の大きさを変える" : "この形状または選択中のキーでは拡縮できません"))
        state.gizmoOp = GizmoOp::Scale;

    const bool worldSpace = ctx.gizmoSpace == EditorContext::GizmoSpace::World;
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::SmallButton(worldSpace ? "World##fe3d_space" : "Local##fe3d_space"))
        ctx.gizmoSpace = worldSpace ? EditorContext::GizmoSpace::Local : EditorContext::GizmoSpace::World;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ギズモ空間: %s (Q で切替)", worldSpace ? "World" : "Local");
    ImGui::SameLine();

    ImGui::BeginDisabled();
    ImGui::SmallButton(ctx.gizmoPivot == EditorContext::GizmoPivot::Pivot ? "Pivot##fe3d_pivot"
                                                                         : "Center##fe3d_pivot");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Fluid は 1 部品ずつ操作するため、Pivot と Center は同じ位置です");
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ctx.snapEnabled ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                                                           : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::SmallButton("Snap##fe3d_snap")) ctx.snapEnabled = !ctx.snapEnabled;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ギズモのスナップを切替 (Ctrl で一時有効)。位置は Fluid の正規化単位、回転は度です");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset View##fe3d_reset_view"))
        StartVolumeOrbitSnap(state.volumeOrbit, settings.cameraYawDegrees, 0.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("編集視点を Bake Camera Yaw へ戻す。Fit はパンとズームだけを戻します");
    ImGui::SameLine();

    if (state.volumeBusy)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kBusy3DText), "焼いています (3D の Baker が空くまで 2D を出します)");
    else if (state.volumeSwitchPending)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kBusy3DText), "ソルバーを切り替えています (次のループで適用)");
    else if (state.volumeStale)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kHint3DText), "前のソルバーの絵を出しています");
    else if (state.volumePending)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kBusy3DText), "解いています...");
    else if (activeKey >= 0)
        ImGui::TextDisabled("Key %d を掴んでいます (タイムラインで選び直せます)", activeKey + 1);
    else
        ImGui::TextDisabled("t %.3f s | yaw %.0f° pitch %.0f° | 部品の丸をクリックで選ぶ",
                            state.playhead, state.volumeOrbit.yawDegrees, state.volumeOrbit.pitchDegrees);

    ImGui::Checkbox("Center plane##fe3d_center_plane", &state.showVolumeCenterPlane);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("体積を切らず、中心を通る参照グリッドを重ねます");
    ImGui::SameLine();
    int centerPlane = static_cast<int>(state.volumeCenterPlane);
    static constexpr const char* kCenterPlaneLabels[] = { "XY (Z=0)", "XZ (Y=0)", "YZ (X=0)" };
    ImGui::BeginDisabled(!state.showVolumeCenterPlane);
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::Combo("##fe3d_center_plane_axis", &centerPlane, kCenterPlaneLabels, 3))
        state.volumeCenterPlane = static_cast<VolumeCenterPlane>(std::clamp(centerPlane, 0, 2));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("現在の 3D 視点に追従する参照面: %s", VolumeCenterPlaneLabel(state.volumeCenterPlane));

    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 canvasSize{ (std::max)(avail.x, 32.0f), (std::max)(avail.y, 32.0f) };
    const ImVec2 canvasMax{ canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y };
    /// @note InvisibleButton にしないのは、ImGuizmo は «どの ImGui 項目にも乗っていない» ときしか掴めない
    /// @note       (CanActivate が IsAnyItemHovered を見る) ため。当たり判定を持たない Dummy で場所だけ取り、
    /// @note       視点操作は矩形との当たりで自前に見る。
    ImGui::Dummy(canvasSize);
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(canvasMin, canvasMax);
    const asset::VolumeFlipbookCamera previewBasis =
        MakeVolumePreviewBasis(settings, state.volumeOrbit.yawDegrees, state.volumeOrbit.pitchDegrees);
    const VolumeGizmoCamera camera = MakeVolumePreviewCamera(settings, previewBasis);
    const bool orientationOwnsInput =
        UpdateVolumeOrientationGizmo(state, settings, canvasMin, canvasSize, hovered);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(canvasMin, canvasMax, true);
    drawList->AddRectFilled(canvasMin, canvasMax, kCanvas3DColor);

    const ImVec2 canvasCenter{ canvasMin.x + canvasSize.x * 0.5f, canvasMin.y + canvasSize.y * 0.5f };
    const float side = (std::max)((std::min)(canvasSize.x, canvasSize.y) * 0.96f * state.zoom, 8.0f);
    /// @note 次のフレームのプレビュー解像度はこの大きさで決まる (PreviewTileSize)。
    state.volumeViewSide = side;
    const ImVec2 squareMin{ canvasCenter.x + state.pan.x - side * 0.5f, canvasCenter.y + state.pan.y - side * 0.5f };
    const ImVec2 squareMax{ squareMin.x + side, squareMin.y + side };

    void* rawId = nullptr;
    if (ctx.fluidBake != nullptr && ctx.imguiRenderer != nullptr && ctx.resources != nullptr && !state.volumeBusy) {
        const auto target = ctx.fluidBake->VolumeBaker().PreviewTarget();
        if (target.IsValid()) rawId = ctx.imguiRenderer->GetImTextureID(target, *ctx.resources, 0);
    }
    if (rawId != nullptr) {
        drawList->AddImage(widgets::ToImTextureID(rawId), squareMin, squareMax, { 0.0f, 0.0f },
                           { kColorTileU1, 1.0f });
        drawList->AddRect(squareMin, squareMax, IM_COL32(255, 255, 255, 48));
    } else {
        /// @note 3D をまだ出せない (焼きに取られている・レンダラーが無い・切り替えを待っている)。
        /// @note       ビューポートを空にはしない。
        DrawFluidPreviewSquare(drawList, state, squareMin, side,
                               state.volumeSwitchPending ? "ソルバーを切り替えています (次のループで適用)"
                                                         : "3D を待っています...");
    }

    /// @note 切り替えに失敗すると «黙って前の絵のまま» になる。理由は共有 Baker しか知らないので、
    /// @note       Volume Flipbook Baker パネルを開いていなくてもここで読めるようにする。
    if (!state.volumeBusy && ctx.fluidBake != nullptr) {
        const std::string& note = ctx.fluidBake->VolumePreviewNote();
        if (!note.empty()) {
            const ImU32 color = ctx.fluidBake->VolumePreviewNoteIsFailure() ? kWarn3DText : kBusy3DText;
            drawList->AddText({ squareMin.x + 6.0f, squareMin.y + 6.0f }, color, note.c_str());
        }
    }

    FluidSelection pickTarget;
    drawList->PushClipRect(squareMin, squareMax, true);
    DrawVolumeSpatialGuides(drawList, camera, squareMin, side);
    if (state.showVolumeCenterPlane)
        DrawVolumeCenterPlaneGuide(drawList, camera, squareMin, side, state.volumeCenterPlane);
    const bool mouseInImage = io.MousePos.x >= squareMin.x && io.MousePos.x <= squareMax.x
                           && io.MousePos.y >= squareMin.y && io.MousePos.y <= squareMax.y;
    if (state.showOverlays)
        pickTarget = DrawVolumePartMarkers(drawList, state, camera, squareMin, side, io.MousePos, solverTime,
                                           mouseInImage && !orientationOwnsInput);
    drawList->AddRect(squareMin, squareMax, IM_COL32(255, 255, 255, 48));
    drawList->PopClipRect();
    DrawVolumeOrientationGizmo(drawList, state, camera, canvasMin, canvasSize);
    drawList->PopClipRect();

    /// @name ギズモ
    bool overGizmo = false;
    bool usingGizmo = false;
    const GizmoOp op = state.gizmoOp;
    const bool opUsable = pose.valid
        && (op == GizmoOp::Translate || (op == GizmoOp::Rotate && pose.canRotate && activeKey < 0)
            || (op == GizmoOp::Scale && pose.canScale && activeKey < 0));
    const bool allowPartGizmo = !orientationOwnsInput || state.gizmoActive;
    if (state.showOverlays && opUsable && allowPartGizmo) {
        math::Matrix4 worldRow = math::Matrix4::Translate(pose.position);
        if (op == GizmoOp::Rotate || (ctx.gizmoSpace == EditorContext::GizmoSpace::Local && pose.canRotate))
            worldRow = worldRow * GizmoBasisFromDirection(pose.direction);
        if (op == GizmoOp::Scale) worldRow = worldRow * math::Matrix4::Scale(pose.size);

        math::Matrix4 viewCol = math::Matrix4::Transpose(camera.viewRow);
        math::Matrix4 projCol = math::Matrix4::Transpose(camera.projRow);
        math::Matrix4 worldCol = math::Matrix4::Transpose(worldRow);

        /// @note ID を積むと «同じフレームに 2 つ目のギズモ» (Scene View) と掴んでいる状態を取り違えない。
        ImGuizmo::PushID("fluid_editor_3d");
        ImGuizmo::SetDrawlist();
        ImGuizmo::Enable(true);
        ImGuizmo::SetOrthographic(true);
        ImGuizmo::SetRect(squareMin.x, squareMin.y, side, side);
        const ImGuizmo::OPERATION operation = op == GizmoOp::Rotate ? ImGuizmo::ROTATE
                                            : op == GizmoOp::Scale  ? ImGuizmo::SCALE
                                                                    : ImGuizmo::TRANSLATE;
        const ImGuizmo::MODE mode = ctx.gizmoSpace == EditorContext::GizmoSpace::World
            ? ImGuizmo::WORLD
            : ImGuizmo::LOCAL;
        const float snapValue = op == GizmoOp::Rotate ? ctx.snapRot
                              : op == GizmoOp::Scale  ? ctx.snapScale
                                                      : ctx.snapPos;
        const float snap[3] = { snapValue, snapValue, snapValue };
        const bool snapActive = ctx.snapEnabled || io.KeyCtrl;
        const bool manipulated = ImGuizmo::Manipulate(&viewCol.m[0][0], &projCol.m[0][0], operation, mode,
                                                      &worldCol.m[0][0], nullptr, snapActive ? snap : nullptr);
        overGizmo = ImGuizmo::IsOver();
        usingGizmo = ImGuizmo::IsUsing();
        ImGuizmo::PopID();

        if (usingGizmo && !state.gizmoActive) {
            state.gizmoActive = true;
            state.gizmoUndoLabel = GizmoUndoLabel(op);
            /// @note 再生を止めないのは、動いている絵を見ながら位置を詰めたいため。掴んだ瞬間に止まると
            /// @note       «その一瞬の姿» でしか合わせられない。再生を止めるのは «時間そのものを操る操作»
            /// @note       (スクラブ・コマ送り・先頭へ) だけにしてある。
            document.BeginInteractiveEdit();
        }
        if (manipulated && state.gizmoActive && document.InInteractiveEdit()) {
            fluid::FluidRecipe working = recipe;
            ApplyGizmoEdit(working, document.selection, activeKey, op, math::Matrix4::Transpose(worldCol), solverTime);
            document.ApplyInteractive(working);
        }
        /// @note 通常は EndStaleDrags が閉じる。ここは «掴んだまま何も動かさずに離した» の保険。
        if (!usingGizmo && state.gizmoActive) {
            const char* label = state.gizmoUndoLabel;
            state.gizmoActive = false;
            if (document.InInteractiveEdit()) document.EndInteractiveEdit(ctx, label);
        }
    } else if (state.showOverlays && pose.valid && op != GizmoOp::Translate) {
        const char* reason = activeKey >= 0 ? "キーを選んでいる間は Move だけです"
                                            : "この部品にはこの操作がありません";
        const ImVec2 textSize = ImGui::CalcTextSize(reason);
        drawList->AddText({ canvasCenter.x - textSize.x * 0.5f, squareMax.y - textSize.y - 6.0f }, kHint3DText, reason);
    }

    /// @name 選ぶ
    if (hovered && !orientationOwnsInput && !overGizmo && !usingGizmo && !state.gizmoActive
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && pickTarget.kind != FluidSelectionKind::None) {
        document.selection = pickTarget;
        state.selectedKey = -1;
    }

    /// @name 視点 (Fit はパンとズームを戻す。方位は独立 Orbit で、Reset View が Bake Yaw へ戻す)
    if (hovered && !orientationOwnsInput && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) state.panning = true;
    if (state.panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            state.pan.x += io.MouseDelta.x;
            state.pan.y += io.MouseDelta.y;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            state.panning = false;
        }
    }
    if (hovered && !orientationOwnsInput && io.MouseWheel != 0.0f && !usingGizmo) {
        const float before = state.zoom;
        state.zoom = ClampF(state.zoom * std::pow(1.15f, io.MouseWheel), 0.25f, 8.0f);
        const float ratio = state.zoom / before;
        const ImVec2 squareCenter{ canvasCenter.x + state.pan.x, canvasCenter.y + state.pan.y };
        state.pan.x = io.MousePos.x - (io.MousePos.x - squareCenter.x) * ratio - canvasCenter.x;
        state.pan.y = io.MousePos.y - (io.MousePos.y - squareCenter.y) * ratio - canvasCenter.y;
    }
}

}
