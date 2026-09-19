/// @file    VfxAssetSummary.cpp
/// @brief   `.vfx` を一時シーンへ展開して構成を読み取り、Inspector へ表示する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Editor/Util/VfxAssetSummary.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Components/VFXElement.hpp>
#include <Engine/Scene/Components/VFXScreenEffect.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fbzz::editor {

namespace {

/// 配下に何も窓を持たないときに VFXSystem が使う既定の尺 (VFXSystem.cpp と同値)。
constexpr float kVfxFallbackDuration = 1.0f;

std::filesystem::file_time_type ReadVfxWriteTime(const std::string& path)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(path), ec);
    return ec ? std::filesystem::file_time_type{} : time;
}

/// .mat の [particle] は GPU 判定に効く (フリップブック補間・モーションベクター・自己影)。
/// 解決できないときは null を渡す — その 3 つが判定から外れるだけで、残りは同じ答えになる。
const asset::ParticleMaterialSettings* ResolveVfxParticleMaterial(const std::string& materialPath)
{
    if (materialPath.empty()) return nullptr;
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(NormalizeAssetPath(materialPath));
    const auto* material = asset::AssetManager::Get<asset::MaterialAsset>(handle);
    return material != nullptr ? &material->particle : nullptr;
}

std::string CollectVfxScriptNames(scene::GameObject& gameObject)
{
    auto* component = gameObject.GetComponent<scene::ScriptComponent>();
    if (component == nullptr) return {};

    std::string names;
    for (const scene::ScriptEntry& entry : component->scripts) {
        std::string type;
        if (entry.script)          type = entry.script->GetTypeName();
        else if (entry.serialized) type = entry.serialized->type;
        if (type.empty()) continue;
        if (!names.empty()) names += ", ";
        names += type;
    }
    return names;
}

/// @brief 1 オブジェクトを 1 行に畳む。窓の式は VFXSystem::ObjectEndTime と同じものを写す (VFXSystem.cpp:77)。
/// @note あちらはシーンの GameObject に対する評価、ここはアセットを開く前の読み取り。共有すると
///       VFXSystem がエディタ都合の引数を持つことになるため、式だけを揃える。
VfxSummaryEntry MakeVfxEntry(scene::GameObject& gameObject, int depth)
{
    VfxSummaryEntry entry;
    entry.name  = gameObject.name;
    entry.depth = depth;

    if (auto* emitter = gameObject.GetComponent<scene::ParticleEmitter>()) {
        const scene::ParticleEmitterSettings& s = emitter->settings;
        entry.kind         = "Particle";
        entry.materialPath = s.materialPath;
        entry.hasWindow    = true;
        entry.start        = s.startDelay;
        entry.loop         = s.loop;
        if (!s.loop) {
            const float lifetime = s.lifetime * (1.0f + (std::max)(s.lifetimeRandom, 0.0f));
            entry.end = s.startDelay + s.duration + lifetime;
        }

        entry.gpuRequested = s.simulationMode == scene::ParticleSimulationMode::Gpu;
        if (entry.gpuRequested) {
            const auto reason = scene::GetParticleGpuFallbackReason(s, ResolveVfxParticleMaterial(s.materialPath));
            entry.gpuActive = reason == scene::ParticleGpuFallbackReason::None;
            if (!entry.gpuActive) entry.gpuFallbackField = scene::ParticleGpuFallbackFieldName(reason);
        }
    } else if (auto* element = gameObject.GetComponent<scene::VFXElement>()) {
        entry.hasWindow = true;
        entry.start     = element->startDelay;
        entry.loop      = element->loop;
        entry.trigger   = element->trigger;
        if (!element->loop && element->duration > 0.0f)
            entry.end = element->startDelay + element->duration;
    }

    if (auto* trail = gameObject.GetComponent<scene::TrailComponent>()) {
        if (entry.kind.empty()) entry.kind = "Trail";
        entry.hasWindow = true;
        entry.end       = (std::max)(entry.end, trail->duration);
    }
    if (auto* decal = gameObject.GetComponent<scene::DecalComponent>()) {
        if (entry.kind.empty()) entry.kind = "Decal";
        entry.hasWindow = true;
        if (decal->lifetime < 0.0f) entry.loop = true;
        else                        entry.end  = (std::max)(entry.end, decal->lifetime);
    }

    if (entry.kind.empty()) {
        if (gameObject.GetComponent<scene::VFXComponent>())            entry.kind = "VFX (nested)";
        else if (gameObject.GetComponent<scene::LightComponent>())     entry.kind = "Light";
        else if (gameObject.GetComponent<scene::FlowField>())         entry.kind = "Flow Field";
        else if (gameObject.GetComponent<scene::VFXScreenEffect>())    entry.kind = "Screen Effect";
        else if (gameObject.GetComponent<scene::VFXCameraShake>())     entry.kind = "Camera Shake";
        else if (gameObject.GetComponent<scene::VFXTimeScale>())       entry.kind = "Time Scale";
        else if (gameObject.GetComponent<scene::AudioSourceComponent>()) entry.kind = "Audio";
        else if (gameObject.GetComponent<scene::MeshRenderer>())       entry.kind = "Mesh";
    }

    const std::string scripts = CollectVfxScriptNames(gameObject);
    if (!scripts.empty())
        entry.kind = entry.kind.empty() ? scripts : entry.kind + " + " + scripts;
    if (entry.kind.empty()) entry.kind = "Group";

    return entry;
}

void CollectVfxEntries(scene::GameObject& parent, int depth, VfxAssetSummary& summary)
{
    const int childCount = parent.GetChildCount();
    for (int index = 0; index < childCount; ++index) {
        scene::GameObject* child = parent.GetChild(index);
        if (child == nullptr) continue;

        VfxSummaryEntry entry = MakeVfxEntry(*child, depth);
        if (entry.loop && entry.trigger.empty()) summary.endless = true;
        summary.effectiveDuration = (std::max)(summary.effectiveDuration, entry.end);
        summary.entries.push_back(std::move(entry));

        /// @note 入れ子 VFX の内側は自前の時間軸を持つので降りない (VFXTimelinePanel と同じ規約)。
        if (child->GetComponent<scene::VFXComponent>() == nullptr)
            CollectVfxEntries(*child, depth + 1, summary);
    }
}

VfxAssetSummary BuildVfxSummary(const std::string& diskPath)
{
    VfxAssetSummary summary;

    /// @note 一時シーンはこの関数から出さない。ParticleEmitter の runtime は空のままで、
    ///       生かしたまま持ち回ると ParticlePass が GPU ハンドルを掴んで取り残す。
    scene::Scene scene;
    std::vector<scene::EntityID> roots;
    if (!PrefabSerializer::Instantiate(scene, diskPath, roots) || roots.empty()) {
        summary.error = "この .vfx を展開できません (Script DLL のビルド中かもしれません)";
        return summary;
    }

    for (const scene::EntityID id : roots) {
        scene::GameObject* root = scene.GetGameObject(id);
        if (root == nullptr) continue;

        if (auto* vfx = root->GetComponent<scene::VFXComponent>(); vfx != nullptr && !summary.hasRoot) {
            summary.hasRoot         = true;
            summary.rootLoop        = vfx->loop;
            summary.rootPlayOnAwake = vfx->playOnAwake;
            summary.rootAutoDestroy = vfx->autoDestroy;
            summary.rootSpeed       = vfx->speed;
            summary.authoredDuration = vfx->duration;
            if (vfx->loop) summary.endless = true;
        }
        CollectVfxEntries(*root, 0, summary);
    }

    if (summary.authoredDuration > 0.0f) {
        /// @note 尺を明示した VFX は配下がループしていてもその長さで終わる。
        ///       VFXSystem が配下から算出するのは duration <= 0 のときだけ (VFXSystem.cpp:112)。
        summary.effectiveDuration = summary.authoredDuration;
        summary.endless           = summary.rootLoop;
    } else if (summary.effectiveDuration <= 0.0f) {
        summary.effectiveDuration = kVfxFallbackDuration;
    }

    summary.valid = true;
    return summary;
}

/// 展開に失敗した .vfx を選んでいる間、引き直しを空ける間隔 [秒]。
constexpr double kVfxSummaryRetryInterval = 0.5;

struct VfxSummaryCache {
    std::string                     path;
    std::filesystem::file_time_type writeTime{};
    double                          nextRetryTime = 0.0;
    VfxAssetSummary                 summary;
};

VfxSummaryCache g_vfxSummaryCache;
const VfxAssetSummary g_emptyVfxSummary;

std::string FormatVfxSeconds(float seconds)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2fs", seconds);
    return buffer;
}

} // namespace

const VfxAssetSummary& GetVfxAssetSummary(const std::string& diskPath)
{
    if (diskPath.empty()) return g_emptyVfxSummary;

    const auto writeTime = ReadVfxWriteTime(diskPath);
    const bool sameAsset = g_vfxSummaryCache.path == diskPath && g_vfxSummaryCache.writeTime == writeTime;
    if (sameAsset && g_vfxSummaryCache.summary.valid) return g_vfxSummaryCache.summary;

    /// @note 失敗は覚えない。ホットリロードで型が戻ってもファイルは変わらないので、覚えると
    ///       «壊れた .vfx» の表示から抜け出せなくなる。ただし毎フレーム引き直すと展開と
    ///       ログを延々と繰り返すので、間隔だけ空ける。
    if (sameAsset && ImGui::GetTime() < g_vfxSummaryCache.nextRetryTime) return g_vfxSummaryCache.summary;

    g_vfxSummaryCache.path          = diskPath;
    g_vfxSummaryCache.writeTime     = writeTime;
    g_vfxSummaryCache.summary       = BuildVfxSummary(diskPath);
    g_vfxSummaryCache.nextRetryTime = ImGui::GetTime() + kVfxSummaryRetryInterval;
    return g_vfxSummaryCache.summary;
}

void DrawVfxAssetInspector(EditorContext& ctx, const std::string& diskPath)
{
    ImGui::TextDisabled("Type: VFX (Prefab)");

    const bool inPrefabEdit = ctx.InPrefabEditMode();
    const bool playing      = ctx.playMode != nullptr && !ctx.playMode->IsInEditor();

    ImGui::BeginDisabled(playing);
    if (ImGui::Button("Open in Prefab Mode", { -1.0f, 0.0f })) {
        OpArgs args;
        args.Set("path", diskPath);
        InvokeOperator(ctx, "asset.open", args);
    }
    ImGui::EndDisabled();

    if (playing)
        ImGui::TextDisabled("Play 中は開けません (先に停止してください)。");
    else if (inPrefabEdit)
        /// @note 編集中でも切り替えられる。今のプレハブは保存してから閉じる。
        ImGui::TextDisabled("今のプレハブを保存して、こちらへ切り替えます。");
    else
        ImGui::TextDisabled("今のシーンを一時退避して中身を再生します。尺は VFX Timeline で詰めます。");

    const VfxAssetSummary& summary = GetVfxAssetSummary(diskPath);
    if (!summary.valid) {
        ImGui::Separator();
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "%s", summary.error.c_str());
        return;
    }

    ImGui::SeparatorText("Playback");
    if (summary.hasRoot) {
        ImGui::TextDisabled("speed x%.2f / %s%s%s", summary.rootSpeed,
                            summary.rootLoop ? "loop" : "one-shot",
                            summary.rootPlayOnAwake ? " / play on awake" : "",
                            summary.rootAutoDestroy ? " / auto destroy" : "");
        if (summary.endless)
            ImGui::TextDisabled("尺: 無限 (ループする要素がある)");
        else
            ImGui::TextDisabled("尺: %s%s", FormatVfxSeconds(summary.effectiveDuration).c_str(),
                                summary.authoredDuration > 0.0f ? " (指定)" : " (配下から算出)");
    } else {
        /// @note ルートに VFXComponent が無いと VFXSystem が時刻を配らず、何も鳴らない。
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                           "ルートに VFX コンポーネントがありません。");
    }

    ImGui::SeparatorText("Layers");
    if (summary.entries.empty()) {
        ImGui::TextDisabled("子オブジェクトがありません。");
        return;
    }

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##VfxLayers", 3, kFlags)) return;

    ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.32f);
    ImGui::TableSetupColumn("Window", ImGuiTableColumnFlags_WidthStretch, 0.23f);
    ImGui::TableHeadersRow();

    for (const VfxSummaryEntry& entry : summary.entries) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (entry.depth > 0) ImGui::Indent(static_cast<float>(entry.depth) * 10.0f);
        ImGui::TextUnformatted(entry.name.c_str());
        if (entry.depth > 0) ImGui::Unindent(static_cast<float>(entry.depth) * 10.0f);
        if (!entry.materialPath.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", entry.materialPath.c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(entry.kind.c_str());
        if (entry.gpuRequested) {
            ImGui::SameLine();
            if (entry.gpuActive) {
                ImGui::TextDisabled("[GPU]");
            } else {
                /// @note 黙って CPU へ落ちるのが GPU シミュレーションの主な事故なので、
                ///       開く前に «どの設定で落ちたか» まで出す (ParticleGpuSimulation.hpp の趣旨)。
                ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "[CPU]");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("GPU を要求していますが %s のため CPU で回ります",
                                      entry.gpuFallbackField.c_str());
            }
        }

        ImGui::TableSetColumnIndex(2);
        if (!entry.trigger.empty())
            ImGui::TextDisabled("trigger: %s", entry.trigger.c_str());
        else if (entry.loop)
            ImGui::TextDisabled("%s - loop", FormatVfxSeconds(entry.start).c_str());
        else if (entry.hasWindow && entry.end > entry.start)
            ImGui::TextDisabled("%s - %s", FormatVfxSeconds(entry.start).c_str(),
                                FormatVfxSeconds(entry.end).c_str());
        else if (entry.hasWindow)
            /// @note VFXElement の duration <= 0 は「ルートが終わるまで開いたまま」。
            ImGui::TextDisabled("%s - ルート終了", FormatVfxSeconds(entry.start).c_str());
        else
            ImGui::TextDisabled("-");
    }

    ImGui::EndTable();
}

} // namespace fbzz::editor
