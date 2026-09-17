/// @file    FluidEditorViewport3D.cpp
/// @brief   Fluid Editor の 3D ライブプレビュー (共有 Baker のレイマーチ絵) と ImGuizmo による部品操作
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note 共有 Baker (VolumeFlipbookBaker) をそのまま使う: 3D 絵の経路はここだけで、160³ の GPU 資源を
///       2 つ持つ余裕はない。FluidBakeService の 1 つを、焼きが使っていないフレームだけ借りる。
/// @note ギズモの座標系はレシピの正規化単位 [-1,1]³ (焼きの bake 空間と同じ) をそのまま使う。カメラは
///       VolumeRaymarch.hlsl と同じ平行投影を組み直せば、絵とギズモが 1 画素もずれない。
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
///       FluidEditorViewport.cpp と 1 つの翻訳単位に混ざり、定数名がぶつかると再定義になるため。
constexpr ImU32 kCanvas3DColor = IM_COL32(22, 22, 25, 255);
constexpr ImU32 kHint3DText = IM_COL32(200, 200, 205, 160);
constexpr ImU32 kBusy3DText = IM_COL32(255, 200, 80, 255);
constexpr ImU32 kWarn3DText = IM_COL32(255, 110, 90, 255);
constexpr float kMinGizmoSize = 0.01f;
/// レイの出発点 (VolumeRaymarch.hlsl の gCamForward * 4)。箱 [-1,1]³ は前後とも余裕を持って入る。
constexpr float kEyeDistance = 4.0f;
/// プレビュー RT は [色 | 速度 | 6-way + | 6-way -] の 4 枚が横に並ぶ。出すのは先頭の色だけ。
constexpr float kColorTileU1 = 0.25f;

struct VolumeGizmoCamera {
    math::Matrix4 viewRow;
    math::Matrix4 projRow;
    math::Matrix4 viewProjRow;
};

struct GizmoPartPose {
    bool valid = false;
    /// 画面に出ている位置 (center + 動きのずれ。キーを選んでいればそのキーの位置)。
    math::Vector3 position;
    math::Vector3 size{ 1.0f, 1.0f, 1.0f };
    math::Vector3 direction{ 0.0f, 1.0f, 0.0f };
    bool canScale = false;
    bool canRotate = false;
};

/// 画面に出す一辺 (画素) から、プレビューのタイル解像度を決める。
/// @note 画面に合わせるのは、表示サイズよりタイルが小さいと拡大されてテクセルが四角く見えるため
///       (frame_size は «焼くときのコマの大きさ» で表示画素数とは無関係)。64 画素刻みに丸めるのは、
///       この値がプレビューの鍵に入っており、1 画素変わるたび鍵が変わって解き直しになるため。
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
        ///       素通しにすると «切り替えただけ» でセル数が 4.6 倍に跳ね、追いつきの Dispatch が
        ///       GPU のウォッチドッグに掛かるため。160 を使ってよいのは焼き (Begin) だけ。
        settings.volumeResolution = (std::min)(settings.volumeResolution, 128);
        settings.tileSize = PreviewTileSize(state.volumeViewSide, 512);
        break;
    }
    /// @note 見るのは色のタイル 1 枚。6-way と supersampling は見えない所に時間を使うだけ。
    settings.sixWayLightmaps = false;
    settings.supersampling = 1;
    return settings;
}

VolumeGizmoCamera MakeVolumePreviewCamera(const asset::VolumeFlipbookBakeSettings& settings)
{
    const asset::VolumeFlipbookCamera camera = asset::ComputeVolumeFlipbookCamera(settings);
    const float halfExtent = (std::max)(settings.halfExtent, 0.05f);
    const math::Vector3 eye = camera.forward * -kEyeDistance;
    VolumeGizmoCamera out;
    out.viewRow = math::Matrix4::LookAt(eye, eye + camera.forward, camera.up);
    out.projRow = math::Matrix4::Orthographic(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.01f,
                                              kEyeDistance * 2.0f);
    out.viewProjRow = out.projRow * out.viewRow;
    return out;
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

/// +Y を direction に合わせた正規直交基底 (回転ギズモが掴む向き)。
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

/// 部品の中心を小さな丸で示し、一番近いものを返す (クリックで選ぶ)。
FluidSelection DrawVolumePartMarkers(ImDrawList* drawList, const State& state, const VolumeGizmoCamera& camera,
                                     ImVec2 min, float side, ImVec2 mouse, float solverTime)
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
            const float distance = (std::max)(std::fabs(mouse.x - screen.x), std::fabs(mouse.y - screen.y));
            if (distance <= nearestDistance) {
                nearestDistance = distance;
                nearest = FluidSelection{ list, index };
            }
        }
    }
    return nearest;
}

} // namespace

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
    if (ctx.fluidBake == nullptr || ctx.renderer == nullptr || ctx.resources == nullptr) return;

    FluidBakeService& service = *ctx.fluidBake;
    if (!service.IsVolumeBakerFree()) {
        state.volumeBusy = true;
        return;
    }

    /// @note hide / solo は文書の Revision を進めない。2D と同じく表示の通番を混ぜた鍵で解き直しを決める。
    ///       +1 は «まだ組んでいない» の 0 と、版数 0・通番 0 の初回を分けるため。
    const std::uint64_t key = state.document.Revision() * 1000003ull + state.visibilityGeneration + 1ull;
    if (key != state.volumeRecipeKey) {
        state.volumeRecipe = state.document.PreviewRecipe();
        state.volumeRecipeKey = key;
    }

    /// @note ソルバーが入れ替わると絵の中身も入れ替わる。再生の途中でそれをやると «同じ一続きの動き» が
    ///       途中で別物にすり替わるので、折り返し (playhead が戻るフレーム) まで前のソルバーで通す。
    ///       止まっているときは待つ理由が無い ── その場で切り替える。
    const bool wrapped = state.playhead < state.volumeSwitchPlayhead;
    /// @note 期限を切るのは、loop = false のレシピは折り返しが永遠に来ないため (テンプレートの半分がそれ)。
    ///       «折り返しまで待つ» だけだと再生中の切り替えでゲートが開かず絵が出ないままになる。
    ///       待つのは続きの動きを守るためなので、少し待って来なければ諦めて当てる。
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
    (void)service.RecordVolumePreview(ctx, VolumePreviewSettings(state), state.playhead, options, &state.volumeRecipe, key);
    state.volumePending = service.VolumeBaker().PreviewPending();
    state.volumeSwitchPending = service.VolumePreviewSwitchPending();
    state.volumeStale = service.VolumePreviewStale();
}

void DrawViewport3D(EditorContext& ctx, State& state)
{
    FluidDocument& document = state.document;
    const fluid::FluidRecipe& recipe = document.Recipe();
    const ImGuiIO& io = ImGui::GetIO();

    static constexpr const char* kOpLabels[] = { "Move##fe3d_move", "Rotate##fe3d_rotate", "Scale##fe3d_scale" };
    static constexpr const char* kOpTips[] = { "中心を動かす (キーを選んでいればそのキーのずれ)",
                                               "向き (direction) を回す — 円錐・輪・板・風・渦・平面だけ",
                                               "大きさを変える — 力は半径、平面は変えられない" };
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(kOpLabels[i], static_cast<int>(state.gizmoOp) == i))
            state.gizmoOp = static_cast<GizmoOp>(i);
        ImGui::SetItemTooltip("%s", kOpTips[i]);
    }
    ImGui::SameLine();
    const int activeKey = ActiveMotionKey(state);
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
        ImGui::TextDisabled("t %.3f s | 部品の丸をクリックで選ぶ", state.playhead);

    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 canvasSize{ (std::max)(avail.x, 32.0f), (std::max)(avail.y, 32.0f) };
    const ImVec2 canvasMax{ canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y };
    /// @note InvisibleButton にしないのは、ImGuizmo は «どの ImGui 項目にも乗っていない» ときしか掴めない
    ///       (CanActivate が IsAnyItemHovered を見る) ため。当たり判定を持たない Dummy で場所だけ取り、
    ///       視点操作は矩形との当たりで自前に見る。
    ImGui::Dummy(canvasSize);
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(canvasMin, canvasMax);

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
        ///       ビューポートを空にはしない。
        DrawFluidPreviewSquare(drawList, state, squareMin, side,
                               state.volumeSwitchPending ? "ソルバーを切り替えています (次のループで適用)"
                                                         : "3D を待っています...");
    }

    /// @note 切り替えに失敗すると «黙って前の絵のまま» になる。理由は共有 Baker しか知らないので、
    ///       Volume Flipbook Baker パネルを開いていなくてもここで読めるようにする。
    if (!state.volumeBusy && ctx.fluidBake != nullptr) {
        const std::string& note = ctx.fluidBake->VolumePreviewNote();
        if (!note.empty()) {
            const ImU32 color = ctx.fluidBake->VolumePreviewNoteIsFailure() ? kWarn3DText : kBusy3DText;
            drawList->AddText({ squareMin.x + 6.0f, squareMin.y + 6.0f }, color, note.c_str());
        }
    }

    const float solverTime = recipe.output.warmup + state.playhead;
    const asset::VolumeFlipbookBakeSettings settings = VolumePreviewSettings(state);
    const VolumeGizmoCamera camera = MakeVolumePreviewCamera(settings);

    FluidSelection pickTarget;
    if (state.showOverlays)
        pickTarget = DrawVolumePartMarkers(drawList, state, camera, squareMin, side, io.MousePos, solverTime);
    drawList->PopClipRect();

    /// @name ギズモ
    bool overGizmo = false;
    bool usingGizmo = false;
    const GizmoPartPose pose = document.selection.IsPart()
        ? ReadGizmoPose(recipe, document.selection, activeKey, solverTime)
        : GizmoPartPose{};
    const GizmoOp op = state.gizmoOp;
    const bool opUsable = pose.valid
        && (op == GizmoOp::Translate || (op == GizmoOp::Rotate && pose.canRotate && activeKey < 0)
            || (op == GizmoOp::Scale && pose.canScale && activeKey < 0));
    if (state.showOverlays && opUsable) {
        math::Matrix4 worldRow = math::Matrix4::Translate(pose.position);
        if (op == GizmoOp::Scale)
            worldRow = worldRow * math::Matrix4::Scale(pose.size);
        else if (op == GizmoOp::Rotate)
            worldRow = worldRow * GizmoBasisFromDirection(pose.direction);

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
        const bool manipulated = ImGuizmo::Manipulate(&viewCol.m[0][0], &projCol.m[0][0], operation, ImGuizmo::WORLD,
                                                      &worldCol.m[0][0]);
        overGizmo = ImGuizmo::IsOver();
        usingGizmo = ImGuizmo::IsUsing();
        ImGuizmo::PopID();

        if (usingGizmo && !state.gizmoActive) {
            state.gizmoActive = true;
            state.gizmoUndoLabel = GizmoUndoLabel(op);
            /// @note 再生を止めないのは、動いている絵を見ながら位置を詰めたいため。掴んだ瞬間に止まると
            ///       «その一瞬の姿» でしか合わせられない。再生を止めるのは «時間そのものを操る操作»
            ///       (スクラブ・コマ送り・先頭へ) だけにしてある。
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
    if (hovered && !overGizmo && !usingGizmo && !state.gizmoActive
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && pickTarget.kind != FluidSelectionKind::None) {
        document.selection = pickTarget;
        state.selectedKey = -1;
    }

    /// @name 視点 (絵の拡大と位置。カメラの向きは [bake] の Camera Yaw が正本)
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) state.panning = true;
    if (state.panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            state.pan.x += io.MouseDelta.x;
            state.pan.y += io.MouseDelta.y;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            state.panning = false;
        }
    }
    if (hovered && io.MouseWheel != 0.0f && !usingGizmo) {
        const float before = state.zoom;
        state.zoom = ClampF(state.zoom * std::pow(1.15f, io.MouseWheel), 0.25f, 8.0f);
        const float ratio = state.zoom / before;
        const ImVec2 squareCenter{ canvasCenter.x + state.pan.x, canvasCenter.y + state.pan.y };
        state.pan.x = io.MousePos.x - (io.MousePos.x - squareCenter.x) * ratio - canvasCenter.x;
        state.pan.y = io.MousePos.y - (io.MousePos.y - squareCenter.y) * ratio - canvasCenter.y;
    }
}

} // namespace fbzz::editor::fluideditor
