/// @file    SequencePanel.cpp
/// @brief   .sequence のタイムライン編集とスクラブプレビュー
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Editor/Panels/SequencePanel.hpp>
#include "SequenceCurveEditing.hpp"

#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

namespace fbzz::editor {
namespace {

/// @note VFX Editor のタイムラインと同じ刻み。エディタ内で「1 コマ」の意味を揃える。
constexpr float  kScrubStep   = 1.0f / 60.0f;
constexpr float  kRowHeight   = 22.0f;
constexpr float  kLabelWidth  = 172.0f;
constexpr float  kRulerHeight = 20.0f;
constexpr float  kKeyRadius   = 5.0f;
constexpr double kMinBarSpan  = 0.02;

using asset::SequenceTrack;
using asset::SequenceTrackType;

/// @name 列 (channel)
/// @note 1 行に複数系統を重ねて描くのは TransformTrack と PropertyTrack だけ。
/// @note 行を系統ごとに増やすと、位置しか打っていないトラックでも 3 行占有してしまう。

int ChannelCount(SequenceTrackType type)
{
    switch (type) {
    /// @note position / rotation / scale
    case SequenceTrackType::Transform: return 3;
    /// @note float / v2 / v3 / v4 / int / bool
    case SequenceTrackType::Property:  return 6;
    default:                           return 1;
    }
}

const char* ChannelName(SequenceTrackType type, int channel)
{
    if (type == SequenceTrackType::Transform) {
        switch (channel) {
        case 0: return "Position";
        case 1: return "Rotation";
        default: return "Scale";
        }
    }
    if (type == SequenceTrackType::Property) {
        switch (channel) {
        case 0: return "Float";
        case 1: return "Vector2";
        case 2: return "Vector3";
        case 3: return "Vector4";
        case 4: return "Int";
        default: return "Bool";
        }
    }
    return "Items";
}

ImU32 TrackColor(SequenceTrackType type)
{
    switch (type) {
    case SequenceTrackType::Animation:  return IM_COL32(196, 150, 60, 255);
    case SequenceTrackType::Transform:  return IM_COL32(80, 150, 110, 255);
    case SequenceTrackType::Property:   return IM_COL32(90, 130, 190, 255);
    case SequenceTrackType::Activation: return IM_COL32(120, 120, 132, 255);
    case SequenceTrackType::Audio:      return IM_COL32(178, 76, 130, 255);
    case SequenceTrackType::VFX:        return IM_COL32(200, 82, 120, 255);
    case SequenceTrackType::Event:      return IM_COL32(226, 176, 70, 255);
    }
    return IM_COL32(120, 120, 120, 255);
}

/// @note 同じ行に重なる系統を色で見分ける。Transform の 3 軸系統は Inspector の
/// @note Position/Rotation/Scale と同じ並びなので、色の意味を覚え直さずに済む。
ImU32 ChannelColor(SequenceTrackType type, int channel)
{
    if (type == SequenceTrackType::Transform) {
        switch (channel) {
        case 0: return IM_COL32(110, 200, 130, 255);
        case 1: return IM_COL32(230, 165, 80, 255);
        default: return IM_COL32(110, 165, 230, 255);
        }
    }
    return TrackColor(type);
}

/// @note 列の実体へアクセスする唯一の分岐点。「個数」「時刻」「削除」「追加」を種別ごとに書くと
/// @note 同じ switch が 5 つ並び、列を 1 つ足すたびに全部を直す羽目になる。
template<typename Fn>
void WithChannel(SequenceTrack& track, int channel, Fn&& fn)
{
    switch (track.type) {
    case SequenceTrackType::Animation:
        if (channel == 0) fn(track.animationClips);
        break;
    case SequenceTrackType::Transform:
        if (channel == 0)      fn(track.positions);
        else if (channel == 1) fn(track.rotations);
        else if (channel == 2) fn(track.scales);
        break;
    case SequenceTrackType::Property:
        switch (channel) {
        case 0: fn(track.property.floatKeys);   break;
        case 1: fn(track.property.vector2Keys); break;
        case 2: fn(track.property.vector3Keys); break;
        case 3: fn(track.property.vector4Keys); break;
        case 4: fn(track.property.intKeys);     break;
        case 5: fn(track.property.boolKeys);    break;
        default: break;
        }
        break;
    case SequenceTrackType::Activation:
        if (channel == 0) fn(track.ranges);
        break;
    case SequenceTrackType::Audio:
        if (channel == 0) fn(track.audioClips);
        break;
    case SequenceTrackType::VFX:
        if (channel == 0) fn(track.vfxClips);
        break;
    case SequenceTrackType::Event:
        if (channel == 0) fn(track.eventKeys);
        break;
    }
}

template<typename Fn>
void WithChannel(const SequenceTrack& track, int channel, Fn&& fn)
{
    WithChannel(const_cast<SequenceTrack&>(track), channel,
                [&](auto& values) { fn(std::as_const(values)); });
}

template<typename Element>
double ElementStart(const Element& element)
{
    if constexpr (requires { element.time; }) return element.time;
    else                                      return element.start;
}

template<typename Element>
void SetElementStart(Element& element, double value)
{
    if constexpr (requires { element.time; }) element.time  = value;
    else                                      element.start = value;
}

std::size_t ChannelSize(const SequenceTrack& track, int channel)
{
    std::size_t count = 0;
    WithChannel(track, channel, [&](const auto& values) { count = values.size(); });
    return count;
}

/// @note タイムライン上での 1 項目の占有区間。end < 0 なら点 (キー) として描く。
struct ItemSpan {
    double start = 0.0;
    double end   = -1.0;
    [[nodiscard]] bool IsBar() const { return end > start; }
};

ItemSpan SpanOf(const SequenceTrack& track, int channel, std::size_t index, double sequenceDuration)
{
    ItemSpan span;
    switch (track.type) {
    case SequenceTrackType::Animation: {
        if (index >= track.animationClips.size()) break;
        const auto& clip = track.animationClips[index];
        span.start = clip.start;
        /// @note 尺を書いていないクリップは次のクリップの頭まで。最後なら演出の終わりまで。
        /// @note       実際のクリップ長は Animator がロードしないと分からないので、ここでは出さない。
        span.end = clip.duration > 0.0 ? clip.start + clip.duration
                 : (index + 1 < track.animationClips.size()
                        ? track.animationClips[index + 1].start
                        : sequenceDuration);
        break;
    }
    case SequenceTrackType::Activation: {
        if (index >= track.ranges.size()) break;
        span.start = track.ranges[index].start;
        span.end   = track.ranges[index].end;
        break;
    }
    case SequenceTrackType::Audio: {
        if (index >= track.audioClips.size()) break;
        const auto& clip = track.audioClips[index];
        span.start = clip.start;
        /// @note one-shot は「その瞬間に鳴る」だけなので点。帯にすると尺を持つように見える。
        span.end = clip.loop ? (clip.end > clip.start ? clip.end : sequenceDuration) : -1.0;
        break;
    }
    case SequenceTrackType::VFX: {
        if (index >= track.vfxClips.size()) break;
        span.start = track.vfxClips[index].start;
        span.end   = track.vfxClips[index].end > track.vfxClips[index].start
                   ? track.vfxClips[index].end : sequenceDuration;
        break;
    }
    default:
        WithChannel(track, channel, [&](const auto& values) {
            if (index < values.size()) span.start = ElementStart(values[index]);
        });
        break;
    }
    return span;
}

void MoveItem(SequenceTrack& track, int channel, std::size_t index, double newStart)
{
    const double clamped = (std::max)(newStart, 0.0);
    switch (track.type) {
    case SequenceTrackType::Activation:
        if (index < track.ranges.size()) {
            const double length = track.ranges[index].end - track.ranges[index].start;
            track.ranges[index].start = clamped;
            track.ranges[index].end   = clamped + length;
        }
        break;
    case SequenceTrackType::Audio:
        if (index < track.audioClips.size()) {
            const double length = track.audioClips[index].end - track.audioClips[index].start;
            track.audioClips[index].start = clamped;
            if (track.audioClips[index].loop && length > 0.0)
                track.audioClips[index].end = clamped + length;
        }
        break;
    case SequenceTrackType::VFX:
        if (index < track.vfxClips.size()) {
            const double length = track.vfxClips[index].end - track.vfxClips[index].start;
            track.vfxClips[index].start = clamped;
            if (length > 0.0) track.vfxClips[index].end = clamped + length;
        }
        break;
    default:
        WithChannel(track, channel, [&](auto& values) {
            if (index < values.size()) SetElementStart(values[index], clamped);
        });
        break;
    }
}

void ResizeItem(SequenceTrack& track, std::size_t index, double newEnd)
{
    switch (track.type) {
    case SequenceTrackType::Animation:
        if (index < track.animationClips.size()) {
            auto& clip = track.animationClips[index];
            clip.duration = (std::max)(newEnd - clip.start, kMinBarSpan);
        }
        break;
    case SequenceTrackType::Activation:
        if (index < track.ranges.size())
            track.ranges[index].end =
                (std::max)(newEnd, track.ranges[index].start + kMinBarSpan);
        break;
    case SequenceTrackType::Audio:
        if (index < track.audioClips.size())
            track.audioClips[index].end =
                (std::max)(newEnd, track.audioClips[index].start + kMinBarSpan);
        break;
    case SequenceTrackType::VFX:
        if (index < track.vfxClips.size())
            track.vfxClips[index].end =
                (std::max)(newEnd, track.vfxClips[index].start + kMinBarSpan);
        break;
    default:
        break;
    }
}

/// @note 時刻順に並べ直す。サンプリングは std::upper_bound でキーを引くため、キーを隣より後ろへ
/// @note ドラッグしたまま並びが崩れていると二分探索が別のキー区間を返し「打った値と違う姿勢が出る」。
void SortChannel(SequenceTrack& track, int channel)
{
    WithChannel(track, channel, [](auto& values) {
        std::stable_sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
            return ElementStart(a) < ElementStart(b);
        });
    });
}

void EraseItem(SequenceTrack& track, int channel, std::size_t index)
{
    WithChannel(track, channel, [&](auto& values) {
        if (index < values.size())
            values.erase(values.begin() + static_cast<std::ptrdiff_t>(index));
    });
}

/// @note 直近の Transform キーを複製せず、既定値で作る。複製すると「動かないキー」が増え打っても
/// @note 何も変わらない形で詰まるため、値は Inspector で入れる前提にする。
void InsertItem(SequenceTrack& track, int channel, double time)
{
    switch (track.type) {
    case SequenceTrackType::Animation: {
        asset::SequenceAnimationClip clip;
        clip.start = time;
        track.animationClips.push_back(std::move(clip));
        std::stable_sort(track.animationClips.begin(), track.animationClips.end(),
                         [](const auto& a, const auto& b) { return a.start < b.start; });
        break;
    }
    case SequenceTrackType::Activation:
        track.ranges.push_back(asset::SequenceRange{ time, time + 1.0 });
        break;
    case SequenceTrackType::Audio: {
        asset::SequenceAudioClip clip;
        clip.start = time;
        track.audioClips.push_back(std::move(clip));
        break;
    }
    case SequenceTrackType::VFX: {
        asset::SequenceVfxClip clip;
        clip.start = time;
        clip.end   = time + 1.0;
        track.vfxClips.push_back(clip);
        break;
    }
    case SequenceTrackType::Event: {
        asset::SequenceEventKey key;
        key.time = time;
        key.name = "event";
        track.eventKeys.push_back(std::move(key));
        break;
    }
    default:
        WithChannel(track, channel, [&](auto& values) {
            using Element = std::decay_t<decltype(values[0])>;
            Element element{};
            SetElementStart(element, time);
            values.push_back(std::move(element));
            /// @note ElementStart で比べる: この generic lambda は switch の全分岐ぶん実体化されるため、
            /// @note       キー型にしか無い .time を直接書くと clip 型で落ちる。
            std::stable_sort(values.begin(), values.end(),
                             [](const Element& a, const Element& b) {
                                 return ElementStart(a) < ElementStart(b);
                             });
        });
        break;
    }
}

std::string TrackRowLabel(const SequenceTrack& track)
{
    std::string label = track.name.empty()
        ? std::string(asset::SequenceTrackTypeName(track.type)) : track.name;
    if (!track.binding.empty()) label += "  [" + track.binding + "]";
    return label;
}

} /// @note namespace

/// @name ドキュメント

bool SequencePanel::Load(const std::string& path)
{
    asset::SequenceAsset loaded;
    if (!asset::LoadSequenceAsset(path, loaded)) {
        m_error = "Sequence を読み込めませんでした: " + path;
        return false;
    }
    m_asset      = std::move(loaded);
    m_path       = path;
    m_assetPath  = NormalizeAssetPath(path);
    m_dirty      = false;
    m_error.clear();
    m_status.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_selection  = {};
    m_curveTrack = -1;
    m_playhead   = 0.0;
    m_playing    = false;
    ++m_localRevision;
    m_pushedRevision = ~0ull;
    return true;
}

bool SequencePanel::Save()
{
    if (m_path.empty()) return false;
    /// @note ディスクへ出す前に必ず時刻順へ整える。ドラッグ中は並べ替えずに済ませているため、
    /// @note       保存だけが「並びの崩れたまま書かれる」経路になりうる。
    auto saved = m_asset;
    for (auto& track : saved.tracks)
        for (int channel = 0; channel < ChannelCount(track.type); ++channel)
            SortChannel(track, channel);

    if (!asset::SaveSequenceAsset(m_path, saved)) {
        m_error = "Sequence を保存できませんでした: " + m_path;
        return false;
    }
    m_dirty = false;
    m_error.clear();
    m_status = "保存しました: " + m_path;
    AssetDirtyRegistry::MarkClean(m_path);
    return true;
}

void SequencePanel::CloseDocument(EditorContext& ctx)
{
    ReleasePreview(ctx);
    m_asset = {};
    m_apiPreview = false;
    m_playing = false;
    m_previewing = false;
    m_path.clear();
    m_assetPath.clear();
    m_selection = {};
    m_undoStack.clear();
    m_redoStack.clear();
    m_dirty = false;
}

void SequencePanel::PushUndo()
{
    constexpr std::size_t kUndoLimit = 96;
    m_undoStack.push_back(m_asset);
    if (m_undoStack.size() > kUndoLimit) m_undoStack.erase(m_undoStack.begin());
    m_redoStack.clear();
}

void SequencePanel::Undo()
{
    if (m_undoStack.empty()) return;
    m_redoStack.push_back(m_asset);
    m_asset = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    m_selection = {};
    m_curveTrack = -1;
    MarkEdited();
}

void SequencePanel::Redo()
{
    if (m_redoStack.empty()) return;
    m_undoStack.push_back(m_asset);
    m_asset = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    m_selection = {};
    m_curveTrack = -1;
    MarkEdited();
}

double SequencePanel::Duration() const
{
    return (std::max)(m_asset.GetDurationSeconds(), 0.001);
}

double SequencePanel::ApplySnap(double seconds) const
{
    /// @note Shift でスナップを一時反転する (VFX Editor のタイムラインと同じ約束)。
    const bool snapping = m_snap != ImGui::GetIO().KeyShift;
    if (!snapping || m_snapStep <= 0.0f) return seconds;
    const double step = static_cast<double>(m_snapStep);
    return std::round(seconds / step) * step;
}

/// @name プレビュー

scene::SequencePlayerComponent* SequencePanel::ResolvePreviewPlayer(
    EditorContext& ctx, scene::EntityID& outEntity) const
{
    outEntity = scene::EntityID::INVALID;
    if (!ctx.activeScene) return nullptr;

    const auto accept = [&](scene::GameObject* go) -> scene::SequencePlayerComponent* {
        if (!go || !go->IsValid()) return nullptr;
        auto* player = go->GetComponent<scene::SequencePlayerComponent>();
        if (!player) return nullptr;
        outEntity = go->GetID();
        return player;
    };

    if (m_pinnedEntity.IsValid() && ctx.activeScene->IsValid(m_pinnedEntity)) {
        if (auto* player = accept(ctx.activeScene->GetGameObject(m_pinnedEntity))) return player;
    }
    if (auto* player = accept(ctx.GetSelectedGO())) return player;

    /// @note 最後に、この .sequence を指している Player を探す。「今いじっているオブジェクト」を
    /// @note       優先しないと、同じ演出を複数の敵が持つ盤面でプレビュー先が勝手に移る。
    if (m_assetPath.empty()) return nullptr;
    for (auto* go : ctx.activeScene->FindObjectsOfType<scene::SequencePlayerComponent>()) {
        if (!go) continue;
        auto* player = go->GetComponent<scene::SequencePlayerComponent>();
        if (!player || NormalizeAssetPath(player->sequencePath) != m_assetPath) continue;
        outEntity = go->GetID();
        return player;
    }
    return nullptr;
}

void SequencePanel::PushPreview(EditorContext& ctx)
{
    scene::EntityID entity = scene::EntityID::INVALID;
    scene::SequencePlayerComponent* player = ResolvePreviewPlayer(ctx, entity);

    /// @note プレビュー先が移ったら、前の相手を必ず元へ戻してから乗り換える。
    if (m_previewEntity.IsValid() && m_previewEntity != entity) ReleasePreview(ctx);
    if (!player) { m_previewEntity = scene::EntityID::INVALID; return; }

    if (m_pushedRevision != m_localRevision || !player->authoringSequence) {
        /// @note 版数を進めると SequenceSystem が「触ったものを戻して撮り直す」。
        auto preview = m_asset;
        for (auto& track : preview.tracks)
            for (int channel = 0; channel < ChannelCount(track.type); ++channel)
                SortChannel(track, channel);
        player->authoringSequence = std::make_shared<const asset::SequenceAsset>(std::move(preview));
        ++player->authoringRevision;
        m_pushedRevision = m_localRevision;
    }
    player->editorScrubTime = static_cast<float>(m_playhead);
    m_previewEntity = entity;
}

void SequencePanel::ReleasePreview(EditorContext& ctx)
{
    if (!ctx.activeScene || !m_previewEntity.IsValid()) {
        m_previewEntity = scene::EntityID::INVALID;
        return;
    }
    if (ctx.activeScene->IsValid(m_previewEntity)) {
        if (auto* go = ctx.activeScene->GetGameObject(m_previewEntity)) {
            if (auto* player = go->GetComponent<scene::SequencePlayerComponent>()) {
                /// @note scrub を負へ戻すと SequenceSystem が復帰を 1 回だけ流す。
                player->editorScrubTime = -1.0f;
                player->authoringSequence.reset();
                ++player->authoringRevision;
            }
        }
    }
    m_previewEntity = scene::EntityID::INVALID;
    m_pushedRevision = ~0ull;
}

void SequencePanel::OnShutdown()
{
    /// @note ここでは EditorContext を貰えない。scrub は保存されないうえ、
    /// @note       Play/Stop の往復でコンポーネントごと作り直されるため残留しない。
    m_previewEntity = scene::EntityID::INVALID;
}

void SequencePanel::OnUpdate(EditorContext& ctx, float dt)
{
    const bool inPlay = ctx.playMode && !ctx.playMode->IsInEditor();
    if (m_path.empty()) return;
    /// @note Play 中はシーンの権威がゲーム側にある。そこへスクラブを重ねると、
    /// @note       演出が動いているのか自分が動かしているのか区別できなくなる。
    if (inPlay) { m_playing = false; ReleasePreview(ctx); return; }
    if (!m_apiPreview && (!visible || !WasContentRendered())) {
        if (m_previewEntity.IsValid()) ReleasePreview(ctx);
        return;
    }

    if (m_playing) {
        m_playhead += static_cast<double>(dt)
                    * static_cast<double>(m_playSpeed);
        if (m_asset.wrapMode == asset::SequenceWrapMode::Loop) {
            m_playhead = std::fmod(m_playhead, Duration());
        } else if (m_playhead >= Duration()) {
            m_playhead = Duration();
            m_playing  = false;
        }
    }
    if (m_apiPreview && m_previewing && !inPlay) PushPreview(ctx);
    else if (inPlay && m_previewEntity.IsValid()) ReleasePreview(ctx);
}

void SequencePanel::OnAfterEnd(EditorContext& ctx)
{
    /// @note ウィンドウを閉じた / タブが隠れたフレームでもここは呼ばれる。
    /// @note       閉じた瞬間にスクラブを畳まないと、編集用の姿勢がシーンに残ったままになる。
    if (!m_apiPreview && (!visible || !WasContentRendered()) && m_previewEntity.IsValid())
        ReleasePreview(ctx);
}

/// @name ツールバー

void SequencePanel::DrawToolbar(EditorContext& ctx)
{
    std::string path = m_path;
    ImGui::SetNextItemWidth(360.0f);
    if (widgets::AssetPathField("Sequence", path, ".sequence", ctx.projectRoot)) {
        if (!path.empty() && path != m_path) {
            /// @note 別の演出へ移る前に、今出しているスクラブを畳んで姿勢を戻す。
            ReleasePreview(ctx);
            Load(path);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Save")) (void)Save();
    ImGui::SameLine();
    if (ImGui::Button("Reload") && !m_path.empty()) Load(m_path);
    ImGui::SameLine();
    if (ImGui::Button("Close")) CloseDocument(ctx);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_undoStack.empty());
    if (ImGui::Button("Undo")) Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_redoStack.empty());
    if (ImGui::Button("Redo")) Redo();
    ImGui::EndDisabled();

    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.35f, 1.0f), "*未保存");
    }

    if (m_path.empty()) return;

    /// @name 演出そのものの設定
    float duration = static_cast<float>(m_asset.duration);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("Duration", &duration, 0.05f, 0.0f, 600.0f, "%.2fs")) {
        PushUndo();
        m_asset.duration = (std::max)(static_cast<double>(duration), 0.0);
        MarkEdited();
    }
    ImGui::SetItemTooltip("0 のときは最も遅いキーから導出する");

    ImGui::SameLine();
    int wrap = static_cast<int>(m_asset.wrapMode);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("Wrap", &wrap, "Once\0Loop\0HoldEnd\0")) {
        PushUndo();
        m_asset.wrapMode = static_cast<asset::SequenceWrapMode>(wrap);
        MarkEdited();
    }

    ImGui::SameLine();
    int timeMode = static_cast<int>(m_asset.timeMode);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("Time", &timeMode, "Scaled\0Unscaled\0")) {
        PushUndo();
        m_asset.timeMode = static_cast<asset::SequenceTimeMode>(timeMode);
        MarkEdited();
    }

    ImGui::SameLine();
    if (ImGui::Button("+ Track")) ImGui::OpenPopup("##SequenceAddTrack");
    if (ImGui::BeginPopup("##SequenceAddTrack")) {
        for (int i = 0; i <= static_cast<int>(SequenceTrackType::Event); ++i) {
            const auto type = static_cast<SequenceTrackType>(i);
            if (ImGui::MenuItem(asset::SequenceTrackTypeName(type))) AddTrack(type);
        }
        ImGui::EndPopup();
    }
}

void SequencePanel::DrawTransport(EditorContext& ctx)
{
    const bool inPlay = ctx.playMode && !ctx.playMode->IsInEditor();

    if (ImGui::Checkbox("Preview", &m_previewing)) {
        if (!m_previewing) ReleasePreview(ctx);
    }
    ImGui::SetItemTooltip("スクラブした絵をシーンへ出す。合図 (Event / Audio / VFX の発火) は "
                          "巻き戻せないため、プレビューでは鳴らない");

    ImGui::SameLine();
    if (ImGui::Button("|<")) { m_playhead = 0.0; m_playing = false; }
    ImGui::SameLine();
    if (ImGui::Button("<|")) m_playhead = (std::max)(m_playhead - kScrubStep, 0.0);
    ImGui::SameLine();
    if (ImGui::Button(m_playing ? "||" : ">")) m_playing = !m_playing;
    ImGui::SameLine();
    if (ImGui::Button("|>")) m_playhead = (std::min)(m_playhead + kScrubStep, Duration());
    ImGui::SameLine();
    if (ImGui::Button(">|")) { m_playhead = Duration(); m_playing = false; }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("Speed", &m_playSpeed, 0.05f, 0.05f, 4.0f, "x%.2f");

    ImGui::SameLine();
    ImGui::Checkbox("Snap", &m_snap);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("##SnapStep", &m_snapStep, 0.005f, 1.0f / 240.0f, 1.0f, "%.3fs");
    ImGui::SetItemTooltip("Shift でスナップを一時反転");

    ImGui::SameLine();
    ImGui::Text("t = %.2f / %.2f", m_playhead, Duration());

    if (inPlay) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "Play 中はスクラブしません");
    }


}

/// @name タイムライン

void SequencePanel::DrawTimeline(EditorContext& ctx)
{
    (void)ctx;
    const double span = Duration();
    const float availableWidth = (std::max)(ImGui::GetContentRegionAvail().x, 320.0f);
    const float trackWidth = (std::max)(availableWidth - kLabelWidth, 160.0f);
    const float contentHeight = kRulerHeight
        + kRowHeight * static_cast<float>((std::max)(m_asset.tracks.size(), std::size_t{ 1 }));

    ImGui::InvisibleButton("##SequenceCanvas", { availableWidth, contentHeight });
    const ImVec2 origin = ImGui::GetItemRectMin();
    const bool canvasHovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();

    const float trackLeft = origin.x + kLabelWidth;
    const auto timeToX = [&](double t) {
        return trackLeft + static_cast<float>(std::clamp(t / span, 0.0, 1.0)) * trackWidth;
    };
    const auto xToTime = [&](float x) {
        return std::clamp(static_cast<double>((x - trackLeft) / trackWidth), 0.0, 1.0) * span;
    };

    draw->AddRectFilled(origin, { origin.x + availableWidth, origin.y + contentHeight },
                        IM_COL32(20, 23, 30, 255));

    /// @note 目盛り
    for (int i = 0; i <= 10; ++i) {
        const float ratio = static_cast<float>(i) / 10.0f;
        const float x = trackLeft + trackWidth * ratio;
        const bool major = i % 5 == 0;
        draw->AddLine({ x, origin.y }, { x, origin.y + contentHeight },
                      IM_COL32(60, 66, 82, major ? 160 : 70));
        if (major) {
            char tick[16];
            std::snprintf(tick, sizeof(tick), "%.2fs", span * ratio);
            draw->AddText({ x + 3.0f, origin.y + 2.0f }, IM_COL32(150, 158, 175, 255), tick);
        }
    }

    Selection hovered;
    int hoveredKind = -1;

    for (std::size_t t = 0; t < m_asset.tracks.size(); ++t) {
        SequenceTrack& track = m_asset.tracks[t];
        const float rowTop = origin.y + kRulerHeight + kRowHeight * static_cast<float>(t);
        const float barTop = rowTop + 4.0f;
        const float barBottom = rowTop + kRowHeight - 4.0f;
        const float centerY = (barTop + barBottom) * 0.5f;
        const bool trackSelected = m_selection.track == static_cast<int>(t);

        if (trackSelected)
            draw->AddRectFilled({ origin.x, rowTop },
                                { origin.x + availableWidth, rowTop + kRowHeight },
                                IM_COL32(255, 255, 255, 14));

        const std::string label = TrackRowLabel(track);
        const ImU32 labelColor = track.muted ? IM_COL32(110, 114, 124, 255)
                               : trackSelected ? IM_COL32(255, 235, 170, 255)
                                               : IM_COL32(180, 188, 205, 255);
        draw->AddText({ origin.x + 6.0f, rowTop + 4.0f }, labelColor, label.c_str());

        /// @note 行に乗っていれば、項目の上でなくてもそのトラックを指しているものとして扱う。
        /// @note       キーを打つ操作は「その行の、その時刻で右クリック」なので、既存の項目の上でしか
        /// @note       行を掴めないと最初の 1 個が置けない。
        const bool rowHot = canvasHovered && mouse.y >= rowTop && mouse.y < rowTop + kRowHeight;
        if (rowHot) hovered = { static_cast<int>(t), 0, -1 };

        const int channels = ChannelCount(track.type);
        for (int c = 0; c < channels; ++c) {
            const std::size_t count = ChannelSize(track, c);
            const ImU32 color = track.muted ? IM_COL32(90, 94, 104, 255)
                                            : ChannelColor(track.type, c);
            for (std::size_t i = 0; i < count; ++i) {
                const ItemSpan itemSpan = SpanOf(track, c, i, span);
                const bool selected = trackSelected && m_selection.channel == c
                                   && m_selection.index == static_cast<int>(i);
                if (itemSpan.IsBar()) {
                    const float left = timeToX(itemSpan.start);
                    const float right = (std::max)(timeToX(itemSpan.end), left + 4.0f);
                    draw->AddRectFilled({ left, barTop }, { right, barBottom }, color, 3.0f);
                    draw->AddRect({ left, barTop }, { right, barBottom },
                                  selected ? IM_COL32(255, 220, 130, 255)
                                           : IM_COL32(18, 20, 26, 200),
                                  3.0f, 0, selected ? 2.0f : 1.0f);
                    if (rowHot && mouse.x >= left - 4.0f && mouse.x <= right + 4.0f) {
                        hovered = { static_cast<int>(t), c, static_cast<int>(i) };
                        /// @note 右端 6px は尺のハンドル。掴み分けはカーソルでも示す。
                        hoveredKind = (mouse.x >= right - 6.0f) ? 1 : 0;
                    }
                } else {
                    const float x = timeToX(itemSpan.start);
                    const bool hot = rowHot && std::fabs(mouse.x - x) <= kKeyRadius + 2.0f;
                    if (hot) {
                        hovered = { static_cast<int>(t), c, static_cast<int>(i) };
                        hoveredKind = 0;
                    }
                    /// @note 菱形。帯と形で区別が付くので、色が同系でも取り違えない。
                    const ImU32 keyColor = (selected || hot)
                        ? IM_COL32(255, 235, 150, 255) : color;
                    draw->AddQuadFilled({ x, centerY - kKeyRadius }, { x + kKeyRadius, centerY },
                                        { x, centerY + kKeyRadius }, { x - kKeyRadius, centerY },
                                        keyColor);
                }
            }
        }
    }

    if (hoveredKind >= 0)
        ImGui::SetMouseCursor(hoveredKind == 1 ? ImGuiMouseCursor_ResizeEW
                                               : ImGuiMouseCursor_ResizeAll);

    /// @note 再生ヘッド
    const float headX = timeToX(m_playhead);
    draw->AddLine({ headX, origin.y }, { headX, origin.y + contentHeight },
                  IM_COL32(255, 96, 96, 220), 1.5f);

    /// @name 入力
    const bool overRuler = canvasHovered && mouse.y < origin.y + kRulerHeight;
    if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_scrubbingRuler = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_scrubbingRuler = false;
    if (m_scrubbingRuler) {
        /// @note 掴んだら離すまで追う。行の上へ外れた瞬間に止まると、掴み直しが要る。
        m_playhead = ApplySnap(xToTime(mouse.x));
        m_playing  = false;
    }

    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overRuler) {
        m_selection = hovered;
        if (hovered.HasItem()) {
            m_drag = hovered;
            m_dragKind = hoveredKind;
            m_dragGrabTime = xToTime(mouse.x);
            const ItemSpan itemSpan = SpanOf(m_asset.tracks[static_cast<std::size_t>(hovered.track)],
                                             hovered.channel,
                                             static_cast<std::size_t>(hovered.index), span);
            m_dragOrigin = hoveredKind == 1 ? itemSpan.end : itemSpan.start;
            m_dragPushedUndo = false;
        }
    }

    if (m_drag.HasItem()) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            /// @note 掴んでいる間は並べ替えない (添字が動くと掴んだ対象が入れ替わる)。
            /// @note       離した時点で 1 回だけ整える。
            SortChannelKeepingSelection(m_drag.track, m_drag.channel, m_dragLastValue);
            m_drag = {};
            m_dragKind = -1;
        } else if (m_drag.track < static_cast<int>(m_asset.tracks.size())) {
            SequenceTrack& track = m_asset.tracks[static_cast<std::size_t>(m_drag.track)];
            const double delta = xToTime(mouse.x) - m_dragGrabTime;
            const double next = ApplySnap((std::max)(m_dragOrigin + delta, 0.0));
            const ItemSpan current = SpanOf(track, m_drag.channel,
                                            static_cast<std::size_t>(m_drag.index), span);
            const double before = m_dragKind == 1 ? current.end : current.start;
            if (std::fabs(next - before) > 1.0e-6) {
                /// @note 1 操作分をまとめて戻せるよう、動き始めた瞬間に 1 回だけ撮る。
                if (!m_dragPushedUndo) { PushUndo(); m_dragPushedUndo = true; }
                if (m_dragKind == 1)
                    ResizeItem(track, static_cast<std::size_t>(m_drag.index), next);
                else
                    MoveItem(track, m_drag.channel,
                             static_cast<std::size_t>(m_drag.index), next);
                MarkEdited();
            }
            /// @note 並べ替え後に選び直すための手掛かり。移動なら新しい開始時刻そのもの。
            m_dragLastValue = m_dragKind == 1
                ? SpanOf(track, m_drag.channel,
                         static_cast<std::size_t>(m_drag.index), span).start
                : next;
            ImGui::SetTooltip(m_dragKind == 1 ? "End %.2fs" : "Start %.2fs", next);
        }
    }

    /// @note 行の右クリックで、その時刻へ項目を足す / トラックを操作する。
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !overRuler) {
        m_selection    = hovered;
        m_contextTrack = hovered.track;
        m_contextTime  = ApplySnap(xToTime(mouse.x));
        m_openTrackMenu = m_contextTrack >= 0;
    }
}

void SequencePanel::DrawTrackContextMenu(EditorContext& ctx)
{
    (void)ctx;
    if (m_openTrackMenu) {
        ImGui::OpenPopup("##SequenceTrackMenu");
        m_openTrackMenu = false;
    }
    if (!ImGui::BeginPopup("##SequenceTrackMenu")) return;

    if (m_contextTrack < 0 || m_contextTrack >= static_cast<int>(m_asset.tracks.size())) {
        ImGui::EndPopup();
        return;
    }
    SequenceTrack& track = m_asset.tracks[static_cast<std::size_t>(m_contextTrack)];

    const int channels = ChannelCount(track.type);
    for (int c = 0; c < channels; ++c) {
        char label[64];
        std::snprintf(label, sizeof(label), "Add %s Key",
                      channels > 1 ? ChannelName(track.type, c) : "");
        if (ImGui::MenuItem(channels > 1 ? label : "Add Item"))
            AddItemAt(track, c, m_contextTime);
    }

    if (m_selection.HasItem() && m_selection.track == m_contextTrack) {
        ImGui::Separator();
        if (ImGui::MenuItem("Delete Item")) {
            PushUndo();
            EraseItem(track, m_selection.channel, static_cast<std::size_t>(m_selection.index));
            m_selection.index = -1;
            MarkEdited();
        }
    }

    ImGui::Separator();
    if (ImGui::MenuItem(track.muted ? "Unmute Track" : "Mute Track")) {
        PushUndo();
        track.muted = !track.muted;
        MarkEdited();
    }
    if (ImGui::MenuItem("Duplicate Track")) {
        PushUndo();
        SequenceTrack copy = track;
        copy.name += " Copy";
        m_asset.tracks.insert(
            m_asset.tracks.begin() + static_cast<std::ptrdiff_t>(m_contextTrack) + 1,
            std::move(copy));
        MarkEdited();
    }
    if (ImGui::MenuItem("Delete Track")) DeleteTrack(m_contextTrack);
    ImGui::EndPopup();
}

/// @name Inspector

void SequencePanel::DrawTrackHeaderFields(SequenceTrack& track)
{
    const auto editString = [&](const char* label, std::string& value) {
        const bool changed = widgets::InputString(label, value);
        SnapshotOnActivate();
        if (changed) MarkEdited();
    };
    const auto editEnum = [&](const char* label, auto& value, const char* items) {
        int current = static_cast<int>(value);
        const bool changed = ImGui::Combo(label, &current, items);
        SnapshotOnActivate();
        if (changed) {
            value = static_cast<std::decay_t<decltype(value)>>(current);
            MarkEdited();
        }
    };

    editString("Name", track.name);
    editString("Binding", track.binding);
    ImGui::SetItemTooltip("SequencePlayerComponent の Bindings と突き合わせるキー。\n"
                          "$self = Player を持つ GameObject 自身。\n"
                          "空にできるのは Event トラックだけ (盤面全体への合図になる)");

    if (ImGui::Checkbox("Muted", &track.muted)) { PushUndo(); MarkEdited(); }
    ImGui::SameLine();
    if (ImGui::Checkbox("Restore On Stop", &track.restoreOnStop)) { PushUndo(); MarkEdited(); }
    ImGui::SetItemTooltip("停止時に、このトラックが触った値を元へ戻す");

    switch (track.type) {
    case SequenceTrackType::Animation:
        editString("Layer", track.layerName);
        ImGui::SetItemTooltip("Animator の追加レイヤー名。Slot はレイヤーごとに 1 本しか無いため、\n"
                              "同じレイヤーでクリップを重ねることはできない");
        break;
    case SequenceTrackType::Transform:
        editEnum("Space", track.space, "Local\0World\0RelativeToStart\0");
        editEnum("Interp", track.interp, "Step\0Linear\0Cubic\0");
        break;
    case SequenceTrackType::Property: {
        editEnum("Target", track.property.targetType, "Component\0Material\0Morph\0");
        editEnum("Value", track.property.valueType,
                 "Float\0Vector2\0Vector3\0Vector4\0Color\0Int\0Bool\0");
        editString("Component", track.property.componentType);
        ImGui::SetItemTooltip("ComponentRegistry の型名か Script の TYPE_NAME");
        editString("Property", track.property.propertyName);
        ImGui::SetItemTooltip("Reflect() の保存キー。Material のときはシェーダーのパラメーター名");
        int slot = track.property.materialSlot;
        const bool slotChanged = ImGui::InputInt("Material Slot", &slot);
        SnapshotOnActivate();
        if (slotChanged) {
            track.property.materialSlot = static_cast<std::int32_t>((std::max)(slot, 0));
            MarkEdited();
        }
        editEnum("Interp##prop", track.property.interp, "Step\0Linear\0Cubic\0");
        break;
    }
    default:
        break;
    }
}

void SequencePanel::DrawSelectedItemFields(SequenceTrack& track)
{
    const auto index = static_cast<std::size_t>(m_selection.index);

    const auto editTime = [&](const char* label, double& value) {
        float seconds = static_cast<float>(value);
        const bool changed = ImGui::DragFloat(label, &seconds, 0.01f, 0.0f, 600.0f, "%.3fs");
        SnapshotOnActivate();
        if (changed && std::isfinite(seconds)) {
            value = (std::max)(static_cast<double>(seconds), 0.0);
            MarkEdited();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            m_pendingSortTrack   = m_selection.track;
            m_pendingSortChannel = m_selection.channel;
            m_pendingSortTime    = value;
        }
    };
    /// @note 値の編集はどれも「掴んだ瞬間に 1 回撮って、変わったら dirty」で同じ形になる。
    const auto edited = [&](bool changed) {
        SnapshotOnActivate();
        if (changed) MarkEdited();
        return changed;
    };
    const auto editString = [&](const char* label, std::string& value) {
        edited(widgets::InputString(label, value));
    };

    switch (track.type) {
    case SequenceTrackType::Animation: {
        if (index >= track.animationClips.size()) break;
        auto& clip = track.animationClips[index];
        editTime("Start", clip.start);
        editTime("Duration", clip.duration);
        ImGui::SetItemTooltip("0 なら次のクリップの頭まで");
        if (widgets::AssetPathField("Source", clip.sourcePath, ".fbx,.anim,.obj,.gltf,.glb", m_projectRoot)) {
            PushUndo();
            MarkEdited();
        }
        editString("Clip", clip.clipName);
        edited(ImGui::DragFloat("Speed", &clip.speed, 0.01f, -4.0f, 4.0f, "%.2f"));
        if (ImGui::Checkbox("Loop", &clip.loop)) { PushUndo(); MarkEdited(); }
        editTime("Clip In", clip.clipIn);
        edited(ImGui::DragFloat("Blend In", &clip.blendIn, 0.005f, 0.0f, 2.0f, "%.3fs"));
        edited(ImGui::DragFloat("Blend Out", &clip.blendOut, 0.005f, 0.0f, 2.0f, "%.3fs"));
        break;
    }
    case SequenceTrackType::Transform: {
        if (m_selection.channel == 1) {
            if (index >= track.rotations.size()) break;
            auto& key = track.rotations[index];
            editTime("Time", key.time);
            math::Vector3 euler = widgets::QuatToEulerDeg(key.value);
            if (edited(ImGui::DragFloat3("Euler", &euler.x, 0.5f)))
                key.value = widgets::EulerDegToQuat(euler);
            break;
        }
        auto& keys = m_selection.channel == 0 ? track.positions : track.scales;
        if (index >= keys.size()) break;
        editTime("Time", keys[index].time);
        edited(ImGui::DragFloat3("Value", &keys[index].value.x, 0.01f));
        break;
    }
    case SequenceTrackType::Property: {
        switch (m_selection.channel) {
        case 0: {
            if (index >= track.property.floatKeys.size()) break;
            editTime("Time", track.property.floatKeys[index].time);
            auto& key = track.property.floatKeys[index];
            float value = key.value;
            if (edited(ImGui::DragFloat("Value", &value, 0.01f)) && std::isfinite(value))
                key.value = value;
            if (track.property.interp == asset::AnimInterp::Cubic) {
                float in = key.inTangent;
                float out = key.outTangent;
                if (edited(ImGui::DragFloat("In slope / s", &in, 0.01f)) && std::isfinite(in))
                    key.inTangent = in;
                if (edited(ImGui::DragFloat("Out slope / s", &out, 0.01f)) && std::isfinite(out))
                    key.outTangent = out;
                if (ImGui::Button("Flat tangents")) {
                    PushUndo();
                    key.inTangent = key.outTangent = 0.0f;
                    MarkEdited();
                }
                ImGui::SameLine();
                if (ImGui::Button("Linear tangents")) {
                    PushUndo();
                    detail::SetFloatCurveLinearTangents(track.property.floatKeys, m_selection.index);
                    MarkEdited();
                }
            }
            break;
        }
        case 1:
            if (index >= track.property.vector2Keys.size()) break;
            editTime("Time", track.property.vector2Keys[index].time);
            edited(ImGui::DragFloat2("Value", &track.property.vector2Keys[index].value.x, 0.01f));
            break;
        case 2:
            if (index >= track.property.vector3Keys.size()) break;
            editTime("Time", track.property.vector3Keys[index].time);
            edited(ImGui::DragFloat3("Value", &track.property.vector3Keys[index].value.x, 0.01f));
            break;
        case 3: {
            if (index >= track.property.vector4Keys.size()) break;
            editTime("Time", track.property.vector4Keys[index].time);
            auto& value = track.property.vector4Keys[index].value;
            const bool isColor = track.property.valueType == asset::AnimValueType::Color;
            edited(isColor ? ImGui::ColorEdit4("Value", &value.x)
                           : ImGui::DragFloat4("Value", &value.x, 0.01f));
            break;
        }
        case 4: {
            if (index >= track.property.intKeys.size()) break;
            editTime("Time", track.property.intKeys[index].time);
            int value = track.property.intKeys[index].value;
            if (edited(ImGui::InputInt("Value", &value)))
                track.property.intKeys[index].value = static_cast<std::int32_t>(value);
            break;
        }
        default:
            if (index >= track.property.boolKeys.size()) break;
            editTime("Time", track.property.boolKeys[index].time);
            if (ImGui::Checkbox("Value", &track.property.boolKeys[index].value)) {
                PushUndo();
                MarkEdited();
            }
            break;
        }
        break;
    }
    case SequenceTrackType::Activation: {
        if (index >= track.ranges.size()) break;
        editTime("Start", track.ranges[index].start);
        editTime("End", track.ranges[index].end);
        break;
    }
    case SequenceTrackType::Audio: {
        if (index >= track.audioClips.size()) break;
        auto& clip = track.audioClips[index];
        editTime("Start", clip.start);
        if (widgets::AssetPathField("Clip", clip.clipPath,
                                    widgets::kAudioClipAssetFilter, m_projectRoot)) {
            PushUndo();
            MarkEdited();
        }
        editString("Bus", clip.bus);
        ImGui::SetItemTooltip("Project Settings > Audio で定義したミキサーバス名");
        edited(ImGui::DragFloat("Volume", &clip.volume, 0.01f, 0.0f, 1.0f, "%.2f"));
        if (ImGui::Checkbox("Loop", &clip.loop)) { PushUndo(); MarkEdited(); }
        if (clip.loop) editTime("End", clip.end);
        break;
    }
    case SequenceTrackType::VFX: {
        if (index >= track.vfxClips.size()) break;
        editTime("Start", track.vfxClips[index].start);
        editTime("End", track.vfxClips[index].end);
        ImGui::SetItemTooltip("0 ならグラフ側の尺に任せる");
        if (ImGui::Checkbox("Restart", &track.vfxClips[index].restart)) { PushUndo(); MarkEdited(); }
        break;
    }
    case SequenceTrackType::Event: {
        if (index >= track.eventKeys.size()) break;
        auto& key = track.eventKeys[index];
        editTime("Time", key.time);
        editString("Name", key.name);
        ImGui::SetItemTooltip("OnSequenceEvent が受け取る名前。\"boss.entry.impact\" のような点付き名");
        int intParam = key.intParam;
        if (edited(ImGui::InputInt("Int", &intParam)))
            key.intParam = static_cast<std::int32_t>(intParam);
        edited(ImGui::DragFloat("Float", &key.floatParam, 0.01f));
        break;
    }
    }
}

void SequencePanel::FitFloatCurve(const SequenceTrack& track)
{
    auto keys = track.property.floatKeys;
    (void)detail::SortFloatCurve(keys, -1);
    m_curveTrack = m_selection.track;
    m_curveEnd = (std::max)(Duration(), 1.0);
    double low = 0.0;
    double high = 0.0;
    for (const auto& key : keys) {
        low = (std::min)(low, static_cast<double>(key.value));
        high = (std::max)(high, static_cast<double>(key.value));
    }
    for (int i = 0; i <= 256; ++i) {
        const double value = asset::SampleFloatKeys(keys, m_curveEnd * i / 256.0, track.property.interp);
        if (!std::isfinite(value)) continue;
        low = (std::min)(low, value);
        high = (std::max)(high, value);
    }
    const double padding = (std::max)((high - low) * 0.15, 0.5);
    m_curveMin = static_cast<float>((std::max)(low - padding, -1.0e30));
    m_curveMax = static_cast<float>((std::min)(high + padding, 1.0e30));
}

void SequencePanel::DrawFloatCurve(SequenceTrack& track)
{
    auto& keys = track.property.floatKeys;
    auto sampledKeys = keys;
    (void)detail::SortFloatCurve(sampledKeys, -1);
    ImGui::Separator();
    ImGui::TextUnformatted("Float curve: drag keys or tangent handles; double-click to add");
    const bool fit = ImGui::Button("Fit curve") || m_curveTrack != m_selection.track;
    ImGui::SameLine();
    if (ImGui::Button("Add at playhead")) {
        PushUndo();
        const float value = asset::SampleFloatKeys(sampledKeys, m_playhead, track.property.interp);
        m_selection.channel = 0;
        m_selection.index = detail::AddFloatCurveKey(keys, ApplySnap(m_playhead), value);
        MarkEdited();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete key") && m_selection.channel == 0 && m_selection.HasItem()) {
        PushUndo();
        EraseItem(track, 0, static_cast<std::size_t>(m_selection.index));
        m_selection.index = -1;
        MarkEdited();
    }
    if (fit) FitFloatCurve(track);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{ (std::max)(ImGui::GetContentRegionAvail().x, 80.0f), 180.0f };
    const double valueSpan = static_cast<double>(m_curveMax) - m_curveMin;
    const auto point = [&](double time, double value) {
        const double x = std::clamp(time / m_curveEnd, -8.0, 8.0);
        const double y = std::clamp((value - m_curveMin) / valueSpan, -8.0, 8.0);
        return ImVec2{ origin.x + static_cast<float>(x) * size.x,
            origin.y + size.y - static_cast<float>(y) * size.y };
    };
    const auto mouseTime = [&]() {
        return std::clamp(static_cast<double>((ImGui::GetIO().MousePos.x - origin.x) / size.x)
                          * m_curveEnd, 0.0, m_curveEnd);
    };
    const auto mouseValue = [&]() {
        return static_cast<float>(m_curveMin
            + (1.0 - (ImGui::GetIO().MousePos.y - origin.y) / size.y) * valueSpan);
    };
    const auto distance = [](ImVec2 a, ImVec2 b) {
        const float x = a.x - b.x;
        const float y = a.y - b.y;
        return x * x + y * y;
    };
    const auto handleTime = [&](std::size_t i, bool incoming) {
        const double gap = incoming ? (i > 0 ? keys[i].time - keys[i - 1].time : 0.0)
                                    : (i + 1 < keys.size() ? keys[i + 1].time - keys[i].time : 0.0);
        return (std::min)(gap / 3.0, m_curveEnd * 0.12);
    };
    const auto handle = [&](std::size_t i, bool incoming) {
        const auto& key = keys[i];
        const double dt = handleTime(i, incoming) * (incoming ? -1.0 : 1.0);
        return point(key.time + dt, key.value + dt * (incoming ? key.inTangent : key.outTangent));
    };

    ImGui::InvisibleButton("##FloatCurveCanvas", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_curveDragKind = -1;
        m_curveUndoPushed = false;
        float best = 81.0f;
        if (m_selection.channel == 0 && m_selection.HasItem()
            && static_cast<std::size_t>(m_selection.index) < keys.size()
            && track.property.interp == asset::AnimInterp::Cubic) {
            const auto i = static_cast<std::size_t>(m_selection.index);
            for (int side = 0; side < 2; ++side) {
                if (handleTime(i, side == 0) <= 0.0) continue;
                const float d = distance(ImGui::GetIO().MousePos, handle(i, side == 0));
                if (d < best) { best = d; m_curveDragKind = side + 1; }
            }
        }
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const float d = distance(ImGui::GetIO().MousePos, point(keys[i].time, keys[i].value));
            if (d >= best) continue;
            best = d;
            m_selection.channel = 0;
            m_selection.index = static_cast<int>(i);
            m_curveDragKind = 0;
        }
        if (m_curveDragKind < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            PushUndo();
            m_selection.channel = 0;
            m_selection.index = detail::AddFloatCurveKey(keys, ApplySnap(mouseTime()), mouseValue());
            MarkEdited();
        }
    }
    if (active && m_curveDragKind >= 0 && m_selection.channel == 0 && m_selection.HasItem()
        && static_cast<std::size_t>(m_selection.index) < keys.size()
        && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        if (!m_curveUndoPushed) { PushUndo(); m_curveUndoPushed = true; }
        if (m_curveDragKind == 0) {
            if (detail::MoveFloatCurveKey(keys, m_selection.index,
                    std::clamp(ApplySnap(mouseTime()), 0.0, m_curveEnd), mouseValue())) MarkEdited();
        } else {
            auto& key = keys[static_cast<std::size_t>(m_selection.index)];
            const bool incoming = m_curveDragKind == 1;
            const double dt = handleTime(static_cast<std::size_t>(m_selection.index), incoming)
                            * (incoming ? -1.0 : 1.0);
            if (dt != 0.0) {
                const float slope = static_cast<float>((static_cast<double>(mouseValue()) - key.value) / dt);
                if (std::isfinite(slope)) {
                    (incoming ? key.inTangent : key.outTangent) = slope;
                    MarkEdited();
                }
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_curveDragKind = -1;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end{ origin.x + size.x, origin.y + size.y };
    draw->AddRectFilled(origin, end, IM_COL32(24, 28, 34, 255));
    draw->PushClipRect(origin, end, true);
    for (int i = 1; i < 4; ++i) {
        const float fraction = i / 4.0f;
        draw->AddLine({ origin.x + size.x * fraction, origin.y },
                      { origin.x + size.x * fraction, end.y }, IM_COL32(55, 60, 68, 255));
        draw->AddLine({ origin.x, origin.y + size.y * fraction },
                      { end.x, origin.y + size.y * fraction }, IM_COL32(55, 60, 68, 255));
    }
    sampledKeys = keys;
    (void)detail::SortFloatCurve(sampledKeys, -1);
    if (!sampledKeys.empty()) {
        float previousValue = asset::SampleFloatKeys(sampledKeys, 0.0, track.property.interp);
        ImVec2 previous = point(0.0, previousValue);
        for (int i = 1; i <= 256; ++i) {
            const double time = m_curveEnd * i / 256.0;
            const float nextValue = asset::SampleFloatKeys(sampledKeys, time, track.property.interp);
            const ImVec2 next = point(time, std::isfinite(nextValue) ? nextValue : 0.0);
            if (std::isfinite(previousValue) && std::isfinite(nextValue))
                draw->AddLine(previous, next, IM_COL32(100, 190, 240, 255), 2.0f);
            previous = next;
            previousValue = nextValue;
        }
    }
    const float playheadX = point(m_playhead, 0.0).x;
    draw->AddLine({ playheadX, origin.y }, { playheadX, end.y }, IM_COL32(240, 200, 80, 255));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const bool selected = m_selection.channel == 0 && static_cast<int>(i) == m_selection.index;
        const ImVec2 keyPoint = point(keys[i].time, keys[i].value);
        if (selected && track.property.interp == asset::AnimInterp::Cubic) {
            for (int side = 0; side < 2; ++side) {
                if (handleTime(i, side == 0) <= 0.0) continue;
                const ImVec2 tip = handle(i, side == 0);
                draw->AddLine(keyPoint, tip, IM_COL32(240, 190, 100, 255));
                draw->AddCircleFilled(tip, 4.0f, IM_COL32(240, 190, 100, 255));
            }
        }
        draw->AddCircleFilled(keyPoint, selected ? 6.0f : 4.0f,
                              selected ? IM_COL32(255, 220, 100, 255) : IM_COL32(140, 205, 240, 255));
    }
    draw->PopClipRect();
    draw->AddRect(origin, end, IM_COL32(90, 100, 115, 255));
    ImGui::TextDisabled("0 .. %.3fs | %.4g .. %.4g | t=%.3fs value=%.5g",
        m_curveEnd, m_curveMin, m_curveMax, m_playhead,
        asset::SampleFloatKeys(sampledKeys, m_playhead, track.property.interp));
}

void SequencePanel::DrawInspector(EditorContext& ctx)
{
    if (!m_selection.HasTrack()
        || m_selection.track >= static_cast<int>(m_asset.tracks.size())) {
        ImGui::TextDisabled("トラックを選ぶと、ここで値を編集できます。");
        ImGui::TextDisabled("右クリック: 項目の追加・削除 / トラックの複製・削除");
        return;
    }

    SequenceTrack& track = m_asset.tracks[static_cast<std::size_t>(m_selection.track)];
    ImGui::TextColored(ImVec4(0.85f, 0.80f, 0.55f, 1.0f), "%s Track",
                       asset::SequenceTrackTypeName(track.type));
    ImGui::Separator();
    DrawTrackHeaderFields(track);

    if (track.type == SequenceTrackType::Property
        && track.property.valueType == asset::AnimValueType::Float)
        DrawFloatCurve(track);
    if (!m_selection.HasItem()) return;
    ImGui::Separator();
    const int channels = ChannelCount(track.type);
    if (channels > 1)
        ImGui::TextDisabled("%s #%d", ChannelName(track.type, m_selection.channel),
                            m_selection.index);
    else
        ImGui::TextDisabled("Item #%d", m_selection.index);
    DrawSelectedItemFields(track);

    /// @note プレビュー先の表示。どのオブジェクトへ出しているのか分からないと、
    /// @note       「絵が変わらない」がバインド漏れなのか値の問題なのか切り分けられない。
    ImGui::Separator();
    if (m_previewEntity.IsValid() && ctx.activeScene) {
        if (auto* go = ctx.activeScene->GetGameObject(m_previewEntity))
            ImGui::TextDisabled("Preview: %s", go->name.c_str());
    } else if (m_previewing) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f),
                           "Preview: SequencePlayerComponent を持つ GameObject がありません");
    }
}

/// @name トラック操作

void SequencePanel::SortChannelKeepingSelection(int track, int channel, double keepTime)
{
    (void)keepTime;
    if (track < 0 || track >= static_cast<int>(m_asset.tracks.size())) return;
    SequenceTrack& target = m_asset.tracks[static_cast<std::size_t>(track)];
    if (m_selection.track == track && m_selection.channel == channel) {
        WithChannel(target, channel, [&](auto& values) {
            m_selection.index = detail::SortCurveSelection(values, m_selection.index,
                [](const auto& value) { return ElementStart(value); });
        });
        return;
    }
    SortChannel(target, channel);
}

void SequencePanel::AddTrack(SequenceTrackType type)
{
    PushUndo();
    SequenceTrack track;
    track.type = type;
    track.name = asset::SequenceTrackTypeName(type);
    /// @note Event 以外はターゲットが要る。空欄のままだと黙って何も起きないので、
    /// @note       最初から埋めておいて「ここを直す」と分かる形にする。
    if (type != SequenceTrackType::Event) track.binding = "$self";
    m_asset.tracks.push_back(std::move(track));
    m_selection = { static_cast<int>(m_asset.tracks.size()) - 1, 0, -1 };
    m_curveTrack = -1;
    MarkEdited();
}

void SequencePanel::DeleteTrack(int index)
{
    if (index < 0 || index >= static_cast<int>(m_asset.tracks.size())) return;
    PushUndo();
    m_asset.tracks.erase(m_asset.tracks.begin() + static_cast<std::ptrdiff_t>(index));
    m_selection = {};
    m_curveTrack = -1;
    MarkEdited();
}

void SequencePanel::AddItemAt(SequenceTrack& track, int channel, double time)
{
    PushUndo();
    InsertItem(track, channel, time);
    MarkEdited();
}

/// @name 本体

void SequencePanel::OnRenderContent(EditorContext& ctx)
{
    m_projectRoot = ctx.projectRoot;

    if (!m_requestedPath.empty()) {
        const std::string requested = std::move(m_requestedPath);
        m_requestedPath.clear();
        if (requested != m_path) {
            ReleasePreview(ctx);
            Load(requested);
        }
    }

    DrawToolbar(ctx);

    if (m_path.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Asset Browser から .sequence をダブルクリックするか、");
        ImGui::TextDisabled("Create > Sequence で新しく作ってください。");
        if (!m_error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.5f, 1.0f), "%s", m_error.c_str());
        return;
    }

    ImGui::Separator();
    DrawTransport(ctx);
    ImGui::Separator();

    if (!m_error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.5f, 1.0f), "%s", m_error.c_str());
    else if (!m_status.empty())
        ImGui::TextDisabled("%s", m_status.c_str());

    const float inspectorHeight = 460.0f;
    const float timelineHeight =
        (std::max)(ImGui::GetContentRegionAvail().y - inspectorHeight, 120.0f);

    ImGui::BeginChild("##SequenceTimeline", { 0.0f, timelineHeight }, true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (m_asset.tracks.empty())
        ImGui::TextDisabled("トラックがありません。ツールバーの [+ Track] で追加してください。");
    else
        DrawTimeline(ctx);
    DrawTrackContextMenu(ctx);
    ImGui::EndChild();

    ImGui::BeginChild("##SequenceInspector", { 0.0f, 0.0f }, true);
    DrawInspector(ctx);
    ImGui::EndChild();

    if (!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z)) Undo();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y)) Redo();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) (void)Save();
        if (ImGui::Shortcut(ImGuiKey_Space)) m_playing = !m_playing;
        if (ImGui::Shortcut(ImGuiKey_Delete) && m_selection.HasItem()
            && m_selection.track < static_cast<int>(m_asset.tracks.size())) {
            PushUndo();
            EraseItem(m_asset.tracks[static_cast<std::size_t>(m_selection.track)],
                      m_selection.channel, static_cast<std::size_t>(m_selection.index));
            m_selection.index = -1;
            MarkEdited();
        }
    }

    if (m_pendingSortTrack >= 0) {
        SortChannelKeepingSelection(m_pendingSortTrack, m_pendingSortChannel, m_pendingSortTime);
        m_pendingSortTrack = -1;
    }

    /// @note スクラブの反映はこのフレームの最後で行う。ツールバーで済ませると、
    /// @note       直後のタイムラインで動かした再生ヘッドが 1 フレーム遅れて絵に出る。
    const bool inPlay = ctx.playMode && !ctx.playMode->IsInEditor();
    if (m_previewing && !inPlay) PushPreview(ctx);
    else if (m_previewEntity.IsValid() && !m_apiPreview) ReleasePreview(ctx);

    /// @note 未保存を保存待ちアセットへ載せる。終了時の一括保存ダイアログがこれを見るため、
    /// @note       載せ忘れると編集だけが黙って捨てられる。
    if (m_dirty)
        AssetDirtyRegistry::Register(m_path, m_assetPath, "Sequence",
                                     [this]() { return Save(); });
}

} /// @note namespace fbzz::editor
