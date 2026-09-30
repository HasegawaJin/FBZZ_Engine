/// @file    FluidInspector.cpp
/// @brief   .fluid (流体エフェクトのレシピ) の Inspector — 要約・焼き・焼いた出力への導線 (編集は Fluid Editor)
/// @author  Hasegawa Jin
/// @date    2026-09-11

#include <Editor/Util/FluidInspector.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/FluidAssetWriters.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

using fluid::FluidKind;
using fluid::FluidShading;

constexpr ImVec4 kErrorColor{ 1.0f, 0.40f, 0.30f, 1.0f };

/// @brief Inspector は編集しない。表示に要るレシピだけを覚え、ディスクが書き換わったら読み直す
/// @brief (書くのは Fluid Editor と AI)。
struct FluidSummaryState {
    std::string path;
    std::filesystem::file_time_type stamp{};
    fluid::FluidRecipe recipe;
    bool loaded = false;
    std::string loadError;

    /// @brief 最後に焼き上がった結果 (2D / 3D)。«Create / Update Particle Material» がこれを写す。
    FluidMaterialSource lastBake;
    bool hasBake = false;
    std::string status;
    bool statusIsError = false;

    /// @brief 隣にある焼いた出力 (表示名, 実パス)。毎フレーム stat しないよう 1 秒ごとに見直す。
    std::vector<std::pair<const char*, std::string>> outputs;
    std::chrono::steady_clock::time_point outputsCheckedAt{};
    bool outputsChecked = false;
};

/// @brief Inspector は同時に 1 つのアセットしか開かないため、状態はファイル内 static で足りる。
FluidSummaryState s_state;

/// @brief 焼きは FluidBakeService のジョブ。別のアセットへ移っても追えるよう、s_state とは別に持つ。
EditorContext* s_context = nullptr;
std::uint32_t s_bakeJobId = 0;
std::string s_bakeJobPath;
std::chrono::steady_clock::time_point s_bakeStarted;

std::string FileNameOf(const std::string& path)
{
    return util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).filename());
}

std::filesystem::file_time_type FileStamp(const std::string& path)
{
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(path), error);
    return error ? std::filesystem::file_time_type{} : stamp;
}

[[nodiscard]] FluidBakeService* BakeService()
{
    return s_context != nullptr ? s_context->fluidBake : nullptr;
}

[[nodiscard]] bool BakeJobActive()
{
    FluidBakeService* service = BakeService();
    if (s_bakeJobId == 0 || service == nullptr) return false;
    const FluidJobStatus* status = service->Find(s_bakeJobId);
    return status != nullptr && !status->Finished();
}

void SetStatus(std::string text, bool isError)
{
    s_state.status = std::move(text);
    s_state.statusIsError = isError;
}

void DrawStatus()
{
    if (s_state.status.empty()) return;
    if (s_state.statusIsError) ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    ImGui::TextWrapped("%s", s_state.status.c_str());
    if (s_state.statusIsError) ImGui::PopStyleColor();
}

void Tooltip(const char* text)
{
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void LoadIfNeeded(const std::string& absPath)
{
    const std::filesystem::file_time_type stamp = FileStamp(absPath);
    if (s_state.path == absPath && stamp == s_state.stamp) return;
    if (s_state.path != absPath) s_state = FluidSummaryState{};
    s_state.path = absPath;
    s_state.stamp = stamp;
    s_state.outputsChecked = false;
    fluid::FluidRecipe recipe;
    std::string error;
    if (asset::LoadFluidRecipe(absPath, recipe, &error)) {
        s_state.recipe = std::move(recipe);
        s_state.loaded = true;
        s_state.loadError.clear();
    } else {
        s_state.loaded = false;
        s_state.loadError = error;
    }
}

[[nodiscard]] int FrameCount(const fluid::FluidRecipe& recipe)
{
    return (std::max)(recipe.output.columns, 1) * (std::max)(recipe.output.rows, 1);
}

[[nodiscard]] const char* ShadingName(const fluid::FluidRecipe& recipe)
{
    if (recipe.kind == FluidKind::Liquid) return "Liquid";
    switch (recipe.render.shading) {
    case FluidShading::Smoke:      return "Smoke";
    case FluidShading::Fire:       return "Fire";
    case FluidShading::Glow:       return "Glow";
    case FluidShading::Distortion: return "Distortion";
    case FluidShading::Liquid:     return "Smoke";
    }
    return "?";
}

/// @name 隣の焼いた出力

/// @brief 名前の規則は FluidBaker (2D: `<stem>`_Flipbook / _MV / _Velocity.png) と VolumeFlipbookBaker (3D: `<stem>` / _mv / _6way*)。
/// @note Windows のファイル名は大文字小文字を区別しないので、2D の _MV と 3D の _mv は同じファイルを指す。
void RefreshSiblingOutputs()
{
    const auto now = std::chrono::steady_clock::now();
    if (s_state.outputsChecked && now - s_state.outputsCheckedAt < std::chrono::seconds(1)) return;
    s_state.outputsChecked = true;
    s_state.outputsCheckedAt = now;
    s_state.outputs.clear();

    const std::string base =
        util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(s_state.path).replace_extension());
    const auto addFirst = [&base](const char* label, std::initializer_list<const char*> suffixes) {
        for (const char* suffix : suffixes) {
            std::string candidate = base + suffix;
            if (util::FileSystem::Exists(candidate)) {
                s_state.outputs.emplace_back(label, std::move(candidate));
                return;
            }
        }
    };
    addFirst("Flipbook", { "_Flipbook.dds", "_Flipbook.png" });
    addFirst("Flipbook (3D)", { ".dds", ".png" });
    addFirst("Motion Vectors", { "_MV.dds", "_MV.png" });
    addFirst("6-way +", { "_6wayP.dds", "_6wayP.png" });
    addFirst("6-way -", { "_6wayN.dds", "_6wayN.png" });
    addFirst("Vector Field", { "_Velocity.png" });
    addFirst("VFX", { ".vfx" });
    if (std::string material = SiblingMaterialPath(s_state.path); util::FileSystem::Exists(material))
        s_state.outputs.emplace_back("Material", std::move(material));
}

void DrawSiblingOutputs()
{
    RefreshSiblingOutputs();
    ImGui::SeparatorText("Baked Outputs");
    if (s_state.outputs.empty()) {
        ImGui::TextDisabled("まだ焼いていません (.fluid の隣に出力がありません)。");
        return;
    }
    if (!ImGui::BeginTable("##outputs", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
    ImGui::TableSetupColumn("##file", ImGuiTableColumnFlags_WidthStretch);
    for (const auto& [label, path] : s_state.outputs) {
        ImGui::PushID(label);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(FileNameOf(path).c_str())) widgets::RequestAssetReveal(NormalizeAssetPath(path), false);
        Tooltip("Asset Browser で見せます。");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/// @name 要約

void SummaryRow(const char* label, const char* format, ...) IM_FMTARGS(2);

void SummaryRow(const char* label, const char* format, ...)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
}

template <typename Part>
int CountEnabled(const std::vector<Part>& parts)
{
    return static_cast<int>(std::count_if(parts.begin(), parts.end(), [](const Part& part) { return part.enabled; }));
}

void DrawRecipeSummary()
{
    const fluid::FluidRecipe& recipe = s_state.recipe;
    ImGui::SeparatorText("Recipe");
    if (ImGui::BeginTable("##summary", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        SummaryRow("Kind", "%s", recipe.kind == FluidKind::Gas ? "Gas (grid solver)" : "Liquid (particles, PBF)");
        SummaryRow("Shading", "%s", ShadingName(recipe));

        const auto partRow = [](const char* label, int count, int enabled, int limit) {
            if (count == enabled) SummaryRow(label, "%d / %d", count, limit);
            else                  SummaryRow(label, "%d / %d  (%d off)", count, limit, count - enabled);
        };
        partRow("Sources", static_cast<int>(recipe.sources.size()), CountEnabled(recipe.sources),
                fluid::kMaxFluidSources);
        partRow("Forces", static_cast<int>(recipe.forces.size()), CountEnabled(recipe.forces), fluid::kMaxFluidForces);
        partRow("Colliders", static_cast<int>(recipe.colliders.size()), CountEnabled(recipe.colliders),
                fluid::kMaxFluidColliders);

        const fluid::FluidBakeSettings& bake = recipe.bake;
        if (bake.mode == fluid::FluidBakeMode::Volume3D)
            SummaryRow("Bake Mode", "3D (Volume)  %d^3  %s%s", bake.volumeResolution,
                       bake.solver == fluid::FluidBakeSolver::Gpu ? "GPU" : "CPU",
                       bake.sixWayLightmaps ? "  + 6-way" : "");
        else
            SummaryRow("Bake Mode", "2D (Flat)");

        const fluid::FluidOutputSettings& output = recipe.output;
        const int frames = FrameCount(recipe);
        SummaryRow("Frame", "%d px  x%d supersampling", output.frameSize, output.supersampling);
        SummaryRow("Frames", "%d x %d = %d  (atlas %d x %d px)", output.columns, output.rows, frames,
                   output.frameSize * output.columns, output.frameSize * output.rows);
        SummaryRow("Duration", "%.2f s  |  %.1f fps%s", output.duration,
                   static_cast<float>(frames) / (std::max)(output.duration, 0.05f), output.loop ? "  |  loop" : "");
        SummaryRow("Extras", "%s%s", output.motionVectors ? "Motion Vectors " : "",
                   recipe.kind == FluidKind::Gas && output.vectorField ? "Vector Field" : "");
        ImGui::EndTable();
    }
    if (recipe.sources.empty() || CountEnabled(recipe.sources) == 0)
        ImGui::TextColored(kErrorColor, "有効な発生源がありません。焼いても何も写りません。");
}

/// @name 焼き

/// @brief 焼いた結果を Particle 用 .mat (.fluid と同名) へ書く。既にあればテクスチャとコマ割りだけ追従させる。
bool WriteParticleMaterial(const std::string& fluidPath, const FluidMaterialSource& bake,
                           std::string& outRelativePath)
{
    const std::string materialDisk = SiblingMaterialPath(fluidPath);
    bool created = false;
    std::string error;
    if (!WriteFluidParticleMaterial(materialDisk, bake, created, error)) {
        SetStatus(error, true);
        return false;
    }
    asset::AssetManager::ReloadPath(materialDisk);
    asset::AssetManager::FlushFailed();
    if (s_context != nullptr) s_context->requestAssetBrowserRefresh = true;
    s_state.outputsChecked = false;
    outRelativePath = NormalizeAssetPath(materialDisk);
    return true;
}

void StartBake()
{
    FluidBakeService* service = BakeService();
    if (service == nullptr) {
        SetStatus("焼きの窓口 (FluidBakeService) が繋がっていません。", true);
        return;
    }
    if (BakeJobActive()) return;
    FluidBakeRequest request;
    request.fluidPath = s_state.path;
    FluidJobError error;
    const std::uint32_t id = service->EnqueueBake(*s_context, request, error);
    if (id == 0) {
        SetStatus(error.message, true);
        return;
    }
    s_bakeJobId = id;
    s_bakeJobPath = s_state.path;
    s_bakeStarted = std::chrono::steady_clock::now();
    SetStatus(s_state.recipe.bake.mode == fluid::FluidBakeMode::Volume3D ? "3D で焼いています…" : "焼いています…",
              false);
}

/// @brief 焼き上がりは次の描画フレームで拾う (テクスチャの差し替えと .mat の追従はサービスが済ませている)。
void PollBake()
{
    FluidBakeService* service = BakeService();
    if (s_bakeJobId == 0 || service == nullptr) return;
    const FluidJobStatus* status = service->Find(s_bakeJobId);
    if (status == nullptr) {
        s_bakeJobId = 0;
        return;
    }
    if (!status->Finished()) return;
    const std::uint32_t id = s_bakeJobId;
    s_bakeJobId = 0;
    /// @note 焼いている間に別のアセットへ移った
    if (s_bakeJobPath != s_state.path) return;

    s_state.outputsChecked = false;
    SetStatus(status->message, status->state != FluidJobState::Done);
    if (status->state != FluidJobState::Done) return;
    if (const asset::FluidBakeResult* flat = service->FindFlatResult(id)) {
        s_state.lastBake = FluidMaterialSource::FromFlat(*flat);
        s_state.hasBake = flat->success;
    } else if (const asset::VolumeFlipbookBakeResult* volume = service->FindVolumeResult(id)) {
        const asset::VolumeFlipbookBakeSettings* settings = service->FindVolumeSettings(id);
        if (settings != nullptr) {
            s_state.lastBake = FluidMaterialSource::FromVolume(*volume, *settings);
            s_state.hasBake = volume->success;
        }
    }
}

void DrawBakeProgress()
{
    FluidBakeService* service = BakeService();
    if (s_bakeJobId == 0 || service == nullptr) return;
    const FluidJobStatus* status = service->Find(s_bakeJobId);
    if (status == nullptr || status->Finished()) return;
    const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_bakeStarted).count();
    const char* stage = status->state == FluidJobState::Queued     ? "  待機中"
                      : status->state == FluidJobState::Encoding   ? "  書き出し中"
                                                                   : "";
    char label[64];
    std::snprintf(label, sizeof(label), "%.0f%%  (%.1f s)%s", status->progress * 100.0f, elapsed, stage);
    ImGui::ProgressBar(status->progress, { -1.0f, 0.0f }, label);
    if (s_bakeJobPath != s_state.path)
        ImGui::TextDisabled("焼いているのは %s です。", FileNameOf(s_bakeJobPath).c_str());
    if (ImGui::SmallButton("Cancel Bake")) (void)service->Cancel(s_bakeJobId);
    Tooltip("2D の焼きは途中で止められないため、書き出しは最後まで続きます (結果は捨てます)。");
}

/// @brief 焼き上がった直後だけ出す。.mat が既にあれば焼きのたびにサービスが追従させているので、ここは «作る» ための入口。
void DrawMaterialButton()
{
    if (!s_state.hasBake) return;
    ImGui::BeginDisabled(BakeJobActive());
    if (ImGui::Button("Create / Update Particle Material", { -1.0f, 0.0f })) {
        std::string materialPath;
        if (WriteParticleMaterial(s_state.path, s_state.lastBake, materialPath)) {
            SetStatus(materialPath + " を書きました。ParticleEmitter の Material へ割り当てて使います。", false);
            widgets::RequestAssetReveal(materialPath, false);
        }
    }
    ImGui::EndDisabled();
    Tooltip("コマ割り・再生モード・FPS・Motion Vector の強さ・ブレンドを焼いた結果どおりに設定します。");
}

void DrawBakeSection()
{
    ImGui::SeparatorText("Bake");
    const bool volumeMode = s_state.loaded && s_state.recipe.bake.mode == fluid::FluidBakeMode::Volume3D;
    ImGui::BeginDisabled(BakeJobActive() || BakeService() == nullptr || !s_state.loaded);
    if (ImGui::Button(volumeMode ? "Bake (3D)" : "Bake (2D)", { -1.0f, 0.0f })) StartBake();
    ImGui::EndDisabled();
    if (volumeMode)
        Tooltip("保存済みのレシピを 3D で解き直してレイマーチで焼き、.fluid の隣へ <名前>.dds / _mv.dds を書きます。\n"
                "Fluid Editor の未保存の変更は入りません。裏で焼くので、その間もエディターは操作できます。");
    else
        Tooltip("保存済みのレシピを解き直し、.fluid の隣へ _Flipbook.png / _MV.png / _Velocity.png を書きます。\n"
                "Fluid Editor の未保存の変更は入りません。裏で焼くので、その間もエディターは操作できます。");
    DrawBakeProgress();
    DrawStatus();
    DrawMaterialButton();
}

} // namespace

void BindFluidInspectorContext(EditorContext* ctx)
{
    s_context = ctx;
}

void DrawFluidAssetInspector(const std::string& absPath,
                             renderer::ResourceManager* /*resources*/,
                             renderer::IImGuiRenderer* /*imguiRenderer*/,
                             std::string* /*volumeBakeRequest*/)
{
    LoadIfNeeded(absPath);
    PollBake();
    ImGui::PushID("fluid_asset_summary");

    ImGui::BeginDisabled(s_context == nullptr);
    if (ImGui::Button("Open in Fluid Editor", { -1.0f, ImGui::GetFrameHeight() * 1.8f }))
        s_context->requestOpenFluidEditor = absPath;
    ImGui::EndDisabled();
    Tooltip("発生源・力・障害物・見た目・出力の編集と、ライブプレビューは Fluid Editor で行います。\n"
            "Asset Browser で .fluid をダブルクリックしても開きます。");

    if (s_state.loaded) {
        DrawRecipeSummary();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("読み込めませんでした: %s", s_state.loadError.c_str());
        ImGui::PopStyleColor();
    }
    DrawBakeSection();
    DrawSiblingOutputs();

    ImGui::PopID();
}

} // namespace fbzz::editor
