/// @file    FluidEditorInternal.hpp
/// @brief   Fluid Editor パネルの中身 (Outliner / Viewport / Timeline) が共有する状態と小道具
/// @author  Hasegawa Jin
/// @date    2026-09-12
#pragma once

#include <Editor/Util/FluidDocument.hpp>
#include <Editor/Util/FluidPartOverlay.hpp>
#include <Editor/Util/FluidPreviewCache.hpp>
#include <Fluid/FluidRecipe.hpp>
#include <Math/Vector3.hpp>
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor {
struct EditorContext;
}

namespace fbzz::editor::fluideditor {

enum class TimelineDragKind : std::uint8_t { None, Scrub, BarBody, BarEnd, Key };

/// ビューポートの見せ方。3D は共有 Baker (VolumeFlipbookBaker) のライブプレビュー。
enum class ViewportMode : std::uint8_t { Flat2D, Volume3D };
/// 3D のギズモで何を変えるか。
enum class GizmoOp : std::uint8_t { Translate, Rotate, Scale };

struct ViewportDrag {
    bool active = false;
    bool moved = false;
    FluidPartHandle handle;
    ImVec2 pressMouse{ 0.0f, 0.0f };
    /// つかんだ点とハンドルの中心のずれ。そのまま置くと押した瞬間にハンドルがカーソルへ飛ぶ。
    ImVec2 grabOffset{ 0.0f, 0.0f };
    /// 最後に当てたときのマウスと Shift。止まっている間も毎フレーム当てると Revision が進み、プレビューが解き直し続ける。
    ImVec2 lastMouse{ 0.0f, 0.0f };
    bool lastShift = false;
    const char* undoLabel = "Move Fluid Part";
};

struct TimelineDrag {
    TimelineDragKind kind = TimelineDragKind::None;
    FluidSelectionKind list = FluidSelectionKind::None;
    int index = -1;
    int keyIndex = -1;
    /// 押した位置のタイムライン時刻 (warmup の後を 0。はみ出しても切らない)。
    float pressTime = 0.0f;
    /// 押した時点の startTime / duration / キーの時刻 (ソルバーの時計)。
    float baseStart = 0.0f;
    float baseDuration = 0.0f;
    std::vector<float> baseKeyTimes;
    /// 量のエンベロープのキーの時刻。タイムラインに菱形としては出さないが、帯を動かしたら一緒に動く
    /// (置いていくと «勢いの落ち方» が部品の出番から外れる)。障害物は量を持たないので空のまま。
    std::vector<float> baseAmountKeyTimes;
    bool moved = false;
    const char* undoLabel = "Edit Fluid Timing";
};

struct State {
    FluidDocument document;
    FluidPreviewCache preview;

    /// プレビューへ最後に渡したもの。hide / solo は Revision を進めないので世代を別に数える。
    fluid::FluidRecipe sentRecipe;
    bool hasSent = false;
    std::uint64_t sentDocRevision = 0;
    std::uint64_t sentVisibilityGeneration = 0;
    std::uint64_t visibilityGeneration = 0;
    std::uint64_t previewSerial = 0;

    float playhead = 0.0f;
    bool playing = false;
    bool loop = true;

    bool checkerBackground = true;
    bool showOverlays = true;
    float zoom = 1.0f;
    ImVec2 pan{ 0.0f, 0.0f };
    bool panning = false;

    /// @name 3D ライブプレビュー
    /// @{
    ViewportMode viewMode = ViewportMode::Flat2D;
    /// 開いた直後の 1 回だけ recipe.bake.mode に合わせる。人が切り替えたらもう触らない。
    bool viewModeChosen = false;
    GizmoOp gizmoOp = GizmoOp::Translate;
    /// 共有 Baker を焼きに取られている / コマを解いている。どちらも 2D の絵で代える。
    bool volumeBusy = false;
    bool volumePending = false;
    /// ソルバーの切り替えが折り返し待ちか / 出ている絵が前のソルバーのものか (表示に使う)。
    bool volumeSwitchPending = false;
    bool volumeStale = false;
    /// 前のフレームに 3D へ渡した再生位置。ループの折り返し (値が戻る) を見つけるためだけに持つ。
    float volumeSwitchPlayhead = 0.0f;
    /// 切り替えを待たせている時間 [秒]。折り返しが来ないレシピ (loop = false) で待ち続けないための期限。
    float volumeSwitchWait = 0.0f;
    /// 前のフレームに 3D の絵を出した一辺 (画面画素)。プレビューのタイルをこれに合わせないと、
    /// 小さく焼いた絵を引き伸ばすことになり «四角い» 絵になる。描く前に決める必要があるので持ち越す。
    float volumeViewSide = 0.0f;
    /// 前のフレームに 2D の絵を出した一辺 (画面画素)。プレビューの画像をこれに合わせないと、
    /// 小さく解いた絵を引き伸ばすことになり «四角い» 絵になる。裏のスレッドが描く前に決める必要があるので持ち越す。
    float previewViewSide = 0.0f;
    /// RecordVolumePreview へ渡すレシピ。hide / solo を反映済み。毎フレーム作り直すと部品の
    /// vector を丸ごと複製するので、鍵が変わったときだけ組み直す (0 = まだ組んでいない)。
    fluid::FluidRecipe volumeRecipe;
    std::uint64_t volumeRecipeKey = 0;
    /// ギズモをつかんでいる間 true (Undo は離したフレームに 1 つ積む)。
    bool gizmoActive = false;
    const char* gizmoUndoLabel = "Move Fluid Part";

    ViewportDrag viewDrag;
    TimelineDrag timelineDrag;
    FluidSelection timelineContextTarget;
    /// タイムラインで選んでいる動きのキー (3D のギズモが動かす対象)。index < 0 なら部品そのもの。
    FluidSelection keyOwner;
    int selectedKey = -1;

    FluidSelection renameTarget;
    char renameBuffer[128] = {};
    bool renameNeedsFocus = false;

    std::string status;
    bool statusIsError = false;

    /// 編集中のプレビューの隣へ «焼き上がり» (.fluid の隣の Atlas) を並べる。
    /// 既定は単独表示 — 並べると 1 枚あたりの絵が半分になるので、要るときだけ人が開く。
    bool compareBaked = false;
    /// @}
};

/// 3 種類の部品 (FluidSource / FluidForce / FluidCollider) は enabled・name・center・startTime・duration・motion を
/// 同じ名前で持つので、種類を問わない処理はこれ 1 本で書く。list / index が不正なら何もせず false。
template <typename Recipe, typename Fn>
bool VisitPart(Recipe& recipe, FluidSelectionKind list, int index, Fn&& fn)
{
    const auto visit = [&](auto& parts) {
        if (index < 0 || index >= static_cast<int>(parts.size())) return false;
        fn(parts[static_cast<std::size_t>(index)]);
        return true;
    };
    switch (list) {
    case FluidSelectionKind::Source:   return visit(recipe.sources);
    case FluidSelectionKind::Force:    return visit(recipe.forces);
    case FluidSelectionKind::Collider: return visit(recipe.colliders);
    default:                           return false;
    }
}

[[nodiscard]] float ClampF(float value, float lo, float hi);
[[nodiscard]] const char* ListTitle(FluidSelectionKind list);
[[nodiscard]] ImU32 ListColor(FluidSelectionKind list, float alpha = 1.0f);
[[nodiscard]] int MaxParts(FluidSelectionKind list);

/// キーの間を直線でつないだ、solverTime (warmup を含むソルバーの時計) での中心からのずれ。
[[nodiscard]] math::Vector3 MotionOffsetAt(const fluid::FluidMotion& motion, float solverTime);
/// hide と solo を合わせた «プレビューに出るか»。IsHidden は solo を含まない。
[[nodiscard]] bool PartShownInPreview(const FluidDocument& document, FluidSelectionKind list, int index);

/// タイムラインの長さ (output.duration。warmup は含まない)。
[[nodiscard]] float TimelineDuration(const fluid::FluidRecipe& recipe);
/// 1 コマの秒数 (焼きと同じ刻み)。
[[nodiscard]] float TimelineFrameDt(const State& state, const fluid::FluidRecipe& recipe);
void StepFrame(State& state, int delta);

/// 編集中のプレビューのコマ数と «いま出しているコマ» の添字。
/// @note ビューポートの見出し・焼き上がりとの突き合わせ・ツールバーの表示が同じ数え方をする必要があるため公開する。
[[nodiscard]] int FluidViewportLiveFrameCount(const State& state, const fluid::FluidRecipe& recipe);
[[nodiscard]] int FluidViewportLiveFrame(const State& state, const fluid::FluidRecipe& recipe, int frames);

void SetStatus(State& state, std::string text, bool isError);
/// 部品の数が減った・選択先が消えたときに添字を有効な範囲へ戻す。
void ClampSelection(State& state);
void FixSelectionAfterRemove(FluidSelection& selection, FluidSelectionKind list, int removedIndex, int newCount);
void FixSelectionAfterMove(FluidSelection& selection, FluidSelectionKind list, int from, int to);

bool RemoveSelectedPart(EditorContext& ctx, State& state);
/// 選んでいる部品へ、再生位置に今のずれでキーを打つ (同じコマにあれば置き換える)。
bool InsertMotionKeyAtPlayhead(EditorContext& ctx, State& state);
void BeginRename(State& state, FluidSelectionKind list, int index);

/// マウスを離したドラッグを閉じる (Undo を 1 つ積む)。文書側が操作中の編集を捨てていたら積まずに畳む。
void EndStaleDrags(EditorContext& ctx, State& state);

/// 選んでいるキーが消えた / 別の部品を選んだら外す。
void ClampKeySelection(State& state);
/// 3D のギズモが動かす «選んでいるキー» の添字。無ければ -1。
[[nodiscard]] int ActiveMotionKey(const State& state);

void DrawOutliner(EditorContext& ctx, State& state);
void DrawViewport(EditorContext& ctx, State& state);
void DrawTimeline(EditorContext& ctx, State& state);

/// 2D のプレビュー (市松 → コマ → 枠) を 1 辺 side の正方形へ描く。3D が使えないときの代わりでもある。
/// コマの細かさは previewViewSide (2D のビューポートが測った一辺) に合わせる — 代役で呼ばれたときに
/// 3D の都合で解像度が動くと、戻ったときに 2D のコマを描き直すことになる。
void DrawFluidPreviewSquare(ImDrawList* drawList, State& state, ImVec2 min, float side, const char* emptyText);

/// 3D のライブプレビューを共有 Baker へ記録する。GPU を積むのでレンダラーのフレーム内
/// (パネルの OnBeforeBegin) から毎フレーム呼ぶこと。
void TickVolumePreview(EditorContext& ctx, State& state);
/// 3D のビューポート (共有 Baker の絵 + ImGuizmo)。DrawViewport から呼ぶ。
void DrawViewport3D(EditorContext& ctx, State& state);

} // namespace fbzz::editor::fluideditor
