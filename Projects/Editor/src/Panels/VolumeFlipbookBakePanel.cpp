/// @file    VolumeFlipbookBakePanel.cpp
/// @brief   Volume Flipbook Baker パネルの ImGui UI とベイク・プレビューの駆動。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Editor/Panels/VolumeFlipbookBakePanel.hpp>

#include "VolumeFlipbookComparePreview.hpp"

#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/Uuid.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fbzz::editor {
namespace {

constexpr int kVolumeResolutions[] = { 32, 48, 64, 96, 128 };
constexpr int kTileSizes[] = { 128, 256, 512 };
constexpr float kPreviewSides[] = { 256.0f, 384.0f, 512.0f };
constexpr ImU32 kWarningColor = IM_COL32(255, 80, 60, 255);
constexpr ImU32 kLightColor = IM_COL32(255, 214, 90, 255);
// 一発ものを最後まで再生したあと、頭へ戻る前に止めておく時間 [秒]。
constexpr float kCompareHoldSeconds = 0.5f;

// 生成物はプロジェクトの Generated 配下へ出し、エンジン同梱の Assets を変更しない
// (Procedural Flipbook Generator と同じ置き場)。
std::string GeneratedDirectory(const std::string& projectRoot, const char* relative)
{
    if (projectRoot.empty()) return relative;
    return projectRoot + "/" + relative;
}

template <std::size_t N>
bool ComboFromList(const char* label, int& value, const int (&items)[N])
{
    char preview[16]{};
    std::snprintf(preview, sizeof(preview), "%d", value);
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (const int item : items) {
            char text[16]{};
            std::snprintf(text, sizeof(text), "%d", item);
            if (ImGui::Selectable(text, item == value)) {
                value = item;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// HDR のリニア色を、グラデーションのバーに塗る表示色へ。
ImU32 ToDisplayColor(const math::Vector3& linear)
{
    const auto encode = [](float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f); };
    return ImGui::ColorConvertFloat4ToU32({ encode(linear.x), encode(linear.y), encode(linear.z), 1.0f });
}

bool DrawRamp(asset::VolumeColorRamp& ramp)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = (std::max)(ImGui::CalcItemWidth(), 64.0f);
    const float height = ImGui::GetFrameHeight();
    constexpr int kSegments = 48;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int i = 0; i < kSegments; ++i) {
        const float t0 = static_cast<float>(i) / kSegments;
        const float t1 = static_cast<float>(i + 1) / kSegments;
        const math::Vector3 color = asset::EvaluateVolumeRamp(ramp, (t0 + t1) * 0.5f);
        drawList->AddRectFilled({ origin.x + width * t0, origin.y }, { origin.x + width * t1, origin.y + height },
                                ToDisplayColor(color));
    }
    ImGui::Dummy({ width, height });

    bool changed = false;
    for (int i = 0; i < asset::kVolumeRampStops; ++i) {
        auto& stop = ramp.stops[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        changed |= ImGui::ColorEdit3("##Color", &stop.color.x,
                                     ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        ImGui::SameLine();
        ImGui::SetNextItemWidth((std::max)(width - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x, 32.0f));
        // 両隣の点を越えさせない。シェーダーは位置が昇順だと決め打ちで区間を探す。
        const float lower = i > 0 ? ramp.stops[static_cast<std::size_t>(i - 1)].position : 0.0f;
        const float upper = i + 1 < asset::kVolumeRampStops ? ramp.stops[static_cast<std::size_t>(i + 1)].position : 1.0f;
        changed |= ImGui::SliderFloat("##Position", &stop.position, lower, (std::max)(lower, upper), "%.2f");
        ImGui::PopID();
    }
    return changed;
}

bool DrawEmitterSettings(asset::VolumeEmitterSettings& e)
{
    bool changed = false;
    changed |= ImGui::Checkbox("Burst", &e.burst);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("オン: 0 コマ目で全部出す一発もの\nオフ: 一定間隔で湧き続ける (Loop できる)");
    changed |= ImGui::DragInt(e.burst ? "Count" : "Count / sec", &e.count, 1.0f, 1, 512);
    changed |= ImGui::DragFloat3("Origin", &e.origin.x, 0.01f, -1.0f, 1.0f);
    changed |= ImGui::DragFloat("Origin Radius", &e.originRadius, 0.005f, 0.0f, 0.5f);
    changed |= ImGui::DragFloat3("Direction", &e.direction.x, 0.01f, -1.0f, 1.0f);
    changed |= ImGui::SliderFloat("Cone Angle", &e.coneAngleDegrees, 0.0f, 180.0f, "%.0f deg");
    changed |= ImGui::DragFloat("Speed", &e.speed, 0.01f, 0.0f, 5.0f);
    changed |= ImGui::SliderFloat("Speed Random", &e.speedRandom, 0.0f, 1.0f);
    changed |= ImGui::DragFloat3("Gravity", &e.gravity.x, 0.01f, -10.0f, 10.0f);
    changed |= ImGui::DragFloat("Drag", &e.drag, 0.01f, 0.0f, 10.0f);
    changed |= ImGui::DragFloat("Lifetime", &e.lifetime, 0.01f, 0.05f, 10.0f, "%.2f s");
    changed |= ImGui::SliderFloat("Lifetime Random", &e.lifetimeRandom, 0.0f, 1.0f);
    changed |= ImGui::DragFloat("Fade In", &e.fadeIn, 0.01f, 0.0f, 5.0f, "%.2f s");
    changed |= ImGui::DragFloat("Fade Out", &e.fadeOut, 0.01f, 0.0f, 5.0f, "%.2f s");
    changed |= ImGui::DragFloat("Radius", &e.radius, 0.002f, 0.005f, 0.5f);
    changed |= ImGui::SliderFloat("Radius Random", &e.radiusRandom, 0.0f, 1.0f);
    changed |= ImGui::DragFloat("Growth", &e.growth, 0.01f, 0.1f, 5.0f, "x%.2f");
    changed |= ImGui::DragFloat("Spin", &e.spin, 0.05f, 0.0f, 20.0f, "%.2f rad/s");
    changed |= ImGui::DragFloat("Density", &e.density, 0.01f, 0.0f, 5.0f);
    changed |= ImGui::SliderFloat("Temperature", &e.temperature, 0.0f, 1.0f);
    changed |= ImGui::DragFloat("Cooling Time", &e.coolingTime, 0.01f, 0.01f, 10.0f, "%.2f s");
    changed |= ImGui::SliderFloat("Liquid", &e.liquid, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = 煙 / 1 = 液体 (表面と反射を持つ)。描き方は Look > Liquid で決めます。");
    changed |= ImGui::SliderFloat("Noise Scale", &e.noiseScale, 0.0f, 1.0f);
    changed |= ImGui::DragFloat("Stretch", &e.stretch, 0.01f, 1.0f, 6.0f);
    changed |= ImGui::DragFloat("Stretch / Speed", &e.stretchPerSpeed, 0.01f, 0.0f, 5.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("進行方向への伸び = Stretch + Stretch/Speed × 速さ。速い液滴ほど細長い筋になります。");
    changed |= ImGui::DragFloatRange2("Color Key", &e.colorKeyMin, &e.colorKeyMax, 0.01f, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("puff ごとにこの範囲から乱数で選び、Look > Albedo Ramp の色を引きます。");
    return changed;
}

std::string DescribeFrameRange(const std::vector<std::uint8_t>& issues, std::uint8_t bit)
{
    int first = -1;
    int last = -1;
    for (int i = 0; i < static_cast<int>(issues.size()); ++i) {
        if ((issues[static_cast<std::size_t>(i)] & bit) == 0) continue;
        if (first < 0) first = i;
        last = i;
    }
    if (first < 0) return {};
    return first == last ? "コマ " + std::to_string(first)
                         : "コマ " + std::to_string(first) + "〜" + std::to_string(last);
}

// ループするエミッター 1 つだけのプレビュー用 .vfx。形式は既存の .vfx (プレハブ TOML) と同じ。
// WHY 手で書くか: PrefabSerializer は実在の Scene から保存するため、使うと編集中のシーンへ
//     一時的な GameObject を足して消すことになる (Undo 履歴と «変更あり» 表示を汚す)。
bool WritePreviewVfx(const std::filesystem::path& file, const std::string& rootName,
                     const std::string& materialPath, float lifetime)
{
    const std::string rootId = util::GenerateUUID();
    const std::string emitterId = util::GenerateUUID();
    char numbers[160]{};
    // 1 粒ずつ、寿命いっぱいでアトラスを最後まで再生させる (Lifetime モード)。
    std::snprintf(numbers, sizeof(numbers), "duration = %.4f\nemitRate = %.4f\nlifetime = %.4f\n",
                  lifetime, 1.0f / lifetime, lifetime);

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "# FBZZ Engine\n"
        << "# Volume Flipbook Baker が書き出したプレビュー用 .vfx。ベイクし直すたびに上書きされる。\n"
        << "[scene]\nformat_version = 1\n\n"
        << "[prefab]\nformat_version = 1\nroot_count = 1\n\n"
        << "[[gameobjects]]\nname = \"" << rootName << "\"\ninstanceId = \"" << rootId << "\"\n"
        << "tag = \"Untagged\"\nlayer = 0\nactive = true\nprefabAssetPath = \"\"\nprefabSourceId = \"\"\n"
        << "parent = \"\"\nparentInstanceId = \"\"\n\n"
        << "[gameobjects.transform]\nposition = [0.0, 0.0, 0.0]\nrotation = [0.0, 0.0, 0.0, 1.0]\n"
        << "scale = [1.0, 1.0, 1.0]\n\n"
        << "[gameobjects.VFXComponent]\nenabled = true\nplayOnAwake = true\nloop = true\nspeed = 1.0\n"
        << "duration = 0.0\nautoDestroy = false\n\n"
        << "[[gameobjects]]\nname = \"Flipbook\"\ninstanceId = \"" << emitterId << "\"\n"
        << "tag = \"Untagged\"\nlayer = 0\nactive = true\nprefabAssetPath = \"\"\nprefabSourceId = \"\"\n"
        << "parent = \"" << rootName << "\"\nparentInstanceId = \"" << rootId << "\"\n\n"
        << "[gameobjects.transform]\nposition = [0.0, 1.0, 0.0]\nrotation = [0.0, 0.0, 0.0, 1.0]\n"
        << "scale = [1.0, 1.0, 1.0]\n\n"
        << "[gameobjects.ParticleEmitter]\ncullingEnabled = false\nlodEnabled = false\n"
        << numbers
        << "loop = true\nmaxParticles = 1\nshape = 0\nsizeStart = 2.0\nsizeEnd = 2.0\nsizeCurvePower = 1.0\n"
        << "lifetimeRandom = 0.0\nemitVelocity = [0.0, 0.0, 0.0]\nvelocitySpread = 0.0\n"
        << "velocityDamping = 0.0\ngravity = [0.0, 0.0, 0.0]\n"
        << "materialPath = \"" << materialPath << "\"\n"
        << "renderMode = 0\nsortMode = 1\nangularVelocityMin = 0.0\nangularVelocityMax = 0.0\n"
        << "colorVariation = 0.0\ncolorStart = [1.0, 1.0, 1.0, 1.0]\ncolorEnd = [1.0, 1.0, 1.0, 1.0]\n"
        << "startDelay = 0.0\n";
    return static_cast<bool>(out);
}

} // namespace

VolumeFlipbookBakePanel::VolumeFlipbookBakePanel()
    : m_compare(std::make_unique<VolumeFlipbookComparePreview>())
{
}

VolumeFlipbookBakePanel::~VolumeFlipbookBakePanel() = default;

void VolumeFlipbookBakePanel::OnInit(EditorContext& ctx)
{
    m_resources = ctx.resources;
    std::snprintf(m_baseName.data(), m_baseName.size(), "%s", "VolumeFlipbook");
}

void VolumeFlipbookBakePanel::OnShutdown()
{
    if (m_resources == nullptr) return;
    m_baker.Release(*m_resources);
    m_compare->Release(*m_resources);
}

void VolumeFlipbookBakePanel::MarkSettingsChanged()
{
    m_previewDirty = true;
    m_framingDirty = true;
}

float VolumeFlipbookBakePanel::BakeDuration() const
{
    return static_cast<float>((std::max)(m_settings.source.frameCount, 1)) * m_settings.source.frameDt;
}

float VolumeFlipbookBakePanel::PreviewWidth(float aspect) const
{
    const float available = ImGui::GetContentRegionAvail().x;
    if (m_previewSize >= 3) return (std::max)(available, 64.0f);
    return (std::min)(kPreviewSides[std::clamp(m_previewSize, 0, 2)] * aspect, (std::max)(available, 64.0f));
}

void VolumeFlipbookBakePanel::OnBeforeBegin(EditorContext& ctx)
{
    if (ctx.renderer == nullptr || ctx.resources == nullptr) return;

    if (m_framingDirty) {
        m_framing = asset::AnalyzeVolumeFraming(m_settings);
        m_framingDirty = false;
    }
    if (m_baker.IsBusy()) {
        m_baker.Tick(*ctx.renderer, *ctx.resources);
        return;
    }
    if (!m_resultHandled) {
        m_resultHandled = true;
        m_lastResult = m_baker.Result();
        m_status = m_lastResult.message;
        m_statusIsError = !m_lastResult.success;
        if (m_lastResult.success) {
            ctx.requestAssetBrowserRefresh = true;
            asset::BakedVolumeFlipbook baked;
            std::string error;
            if (m_baker.TakeBakedFlipbook(baked)) {
                if (m_compare->Upload(*ctx.resources, std::move(baked), error)) {
                    m_compareTime = 0.0f;
                    m_comparePlaying = true;
                    m_selectFlipbookTab = true;
                } else {
                    m_status += "\n" + error;
                }
            }
        }
        // ベイクの最後のコマは表示用の変換をせずに描いてある。プレビューを描き直す。
        m_previewDirty = true;
    }

    const float delta = ImGui::GetIO().DeltaTime;
    if (m_playing) {
        m_previewTime = std::fmod(m_previewTime + delta * m_playSpeed, (std::max)(BakeDuration(), 1.0e-3f));
        m_previewDirty = true;
    }
    if (m_previewDirty) {
        m_previewDirty = false;
        asset::VolumePreviewOptions options;
        options.view = m_view == VolumeView::Alpha ? asset::VolumePreviewView::Alpha : asset::VolumePreviewView::Color;
        options.background = m_background;
        m_baker.RecordPreview(*ctx.renderer, *ctx.resources, m_settings, m_previewTime, options);
    }

    if (m_compare->HasFlipbook()) {
        if (m_comparePlaying) {
            const float fps = (std::max)(m_compareFps, 0.1f);
            const float frames = static_cast<float>(m_compare->FrameCount());
            const float cycle = m_compare->Loops() ? frames / fps : frames / fps + kCompareHoldSeconds;
            m_compareTime = std::fmod(m_compareTime + delta, (std::max)(cycle, 1.0e-3f));
        }
        m_compare->Render(*ctx.renderer, *ctx.resources, m_compareTime, m_compareFps, m_strengthScale, m_background);
    }
}

void VolumeFlipbookBakePanel::OnRenderContent(EditorContext& ctx)
{
    const bool busy = m_baker.IsBusy();
    ImGui::BeginDisabled(busy);
    if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) DrawSourceSettings();
    if (ImGui::CollapsingHeader("Look", ImGuiTreeNodeFlags_DefaultOpen)) DrawLookSettings();
    ImGui::EndDisabled();
    DrawFramingWarnings();

    if (ImGui::BeginTabBar("VolumeFlipbookTabs")) {
        if (ImGui::BeginTabItem("Volume")) {
            DrawVolumeTab(ctx);
            ImGui::EndTabItem();
        }
        const ImGuiTabItemFlags flipbookFlags = m_selectFlipbookTab ? ImGuiTabItemFlags_SetSelected : 0;
        m_selectFlipbookTab = false;
        if (ImGui::BeginTabItem("Flipbook", nullptr, flipbookFlags)) {
            DrawFlipbookTab(ctx);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    DrawBake(ctx);
    DrawApply(ctx);
}

void VolumeFlipbookBakePanel::DrawSourceSettings()
{
    auto& source = m_settings.source;
    bool changed = false;

    if (ImGui::BeginCombo("Preset", source.preset.c_str())) {
        for (const asset::VolumeSourceDesc& desc : asset::VolumeSources()) {
            const bool selected = desc.name == source.preset;
            if (ImGui::Selectable(desc.name.c_str(), selected) && !selected) {
                source.preset = desc.name;
                // 似合う Look (液体の色・炎の明るさ) はソースごとに違う。選び直したら推奨へ寄せる。
                asset::ApplyVolumeSourceLook(m_settings);
                changed = true;
            }
            if (ImGui::IsItemHovered() && !desc.description.empty())
                ImGui::SetTooltip("%s", desc.description.c_str());
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (const asset::VolumeSourceDesc* desc = asset::FindVolumeSource(source.preset); desc == nullptr)
        ImGui::TextColored({ 1.0f, 0.45f, 0.3f, 1.0f }, "登録されていないソースです: %s", source.preset.c_str());
    else if (!desc->description.empty())
        ImGui::TextDisabled("%s", desc->description.c_str());
    int seed = static_cast<int>(source.seed);
    if (ImGui::DragInt("Seed", &seed, 1.0f, 0, 1000000)) {
        source.seed = static_cast<std::uint32_t>((std::max)(seed, 0));
        changed = true;
    }
    changed |= ImGui::DragInt("Frames", &source.frameCount, 1.0f, 2, 256);
    changed |= ImGui::DragInt("Columns (0 = auto)", &m_settings.columns, 1.0f, 0, 64);
    float framesPerSecond = 1.0f / source.frameDt;
    if (ImGui::DragFloat("Frames Per Second", &framesPerSecond, 0.5f, 4.0f, 120.0f, "%.1f")) {
        source.frameDt = 1.0f / std::clamp(framesPerSecond, 4.0f, 120.0f);
        changed = true;
    }
    changed |= ImGui::DragFloat("Start Time (-1 = preset)", &source.startTime, 0.01f, -1.0f, 30.0f, "%.2f");
    if (asset::VolumeSourceCanLoop(source)) {
        changed |= ImGui::Checkbox("Loop", &source.loop);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("最終コマの次が先頭コマに厳密に繋がるよう、湧く間隔を調整します。\n"
                              "マテリアルは FPS モードで再生します。");
    }
    if (source.preset == asset::kVolumeEmitterSourceName
        && ImGui::TreeNodeEx("Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= DrawEmitterSettings(source.emitter);
        ImGui::TreePop();
    }
    changed |= ComboFromList("Volume Resolution", m_settings.volumeResolution, kVolumeResolutions);
    changed |= ComboFromList("Tile Size", m_settings.tileSize, kTileSizes);
    changed |= ImGui::DragFloat("Noise Frequency", &m_settings.noise.frequency, 0.05f, 0.25f, 8.0f);
    changed |= ImGui::SliderFloat("Noise Amplitude", &m_settings.noise.amplitude, 0.0f, 1.0f);
    ImGui::InputText("Output Name", m_baseName.data(), m_baseName.size());

    const asset::FlipbookGrid grid = asset::ComputeFlipbookGrid(source.frameCount, m_settings.columns);
    ImGui::TextDisabled("Atlas: %d x %d frames / %d x %d px", grid.columns, grid.rows,
                        grid.columns * m_settings.tileSize, grid.rows * m_settings.tileSize);
    if (changed) MarkSettingsChanged();
}

void VolumeFlipbookBakePanel::DrawLookSettings()
{
    bool changed = false;
    if (ImGui::SmallButton("Reset Look")) {
        asset::ApplyVolumeSourceLook(m_settings);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("選んでいるソースの推奨 Look へ戻します (カメラと光の向きはそのまま)。");
    changed |= ImGui::SliderFloat("Camera Yaw", &m_settings.cameraYawDegrees, -180.0f, 180.0f, "%.0f deg");
    changed |= ImGui::DragFloat("Framing", &m_settings.halfExtent, 0.01f, 0.3f, 2.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("タイルが写す範囲の半幅。1 で箱 [-1,1] がちょうど収まります。\n"
                          "小さくすると煙が大きく写る代わりに、縁で切れやすくなります。");
    changed |= ImGui::SliderFloat("Light Yaw", &m_settings.lightYawDegrees, -180.0f, 180.0f, "%.0f deg");
    changed |= ImGui::SliderFloat("Light Pitch", &m_settings.lightPitchDegrees, -10.0f, 90.0f, "%.0f deg");
    changed |= ImGui::ColorEdit3("Light Color", &m_settings.lightColor.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
    changed |= ImGui::ColorEdit3("Ambient", &m_settings.ambient.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
    changed |= ImGui::DragFloat("Extinction", &m_settings.extinction, 0.1f, 0.0f, 100.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("煙の濃さ。液体の濃さは Liquid > Extinction です。");
    changed |= ImGui::SliderFloat("Anisotropy", &m_settings.anisotropy, -0.9f, 0.9f);
    changed |= ImGui::DragFloat("Emission", &m_settings.emissionIntensity, 0.1f, 0.0f, 100.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("不透明な炎の芯 (温度 1) の明るさ。煙の濃さ (Extinction) を変えても明るさは変わりません。");
    changed |= ImGui::DragFloat("Exposure", &m_settings.exposure, 0.01f, 0.01f, 8.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("HDR の色を 8bit へ落とすときの倍率。\n"
                          "マテリアルの HDR Emissive に 1/Exposure を入れて明るさを戻します。");
    changed |= ImGui::DragInt("Ray Steps", &m_settings.raySteps, 1.0f, 16, 512);
    changed |= ImGui::DragInt("Shadow Steps", &m_settings.shadowSteps, 1.0f, 1, 64);

    if (ImGui::TreeNode("Albedo Ramp")) {
        ImGui::TextDisabled("puff の Color Key → 煙と液体の色");
        changed |= DrawRamp(m_settings.albedoRamp);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Emission Ramp")) {
        ImGui::TextDisabled("温度 → 発光の色 (左 = 冷えた / 右 = 最も熱い)");
        changed |= DrawRamp(m_settings.emissionRamp);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Liquid")) {
        auto& liquid = m_settings.liquid;
        changed |= ImGui::DragFloat("Threshold", &liquid.threshold, 0.005f, 0.05f, 1.5f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("密度がこの値を跨ぐところを液体の表面にします。");
        changed |= ImGui::DragFloat("Softness", &liquid.softness, 0.002f, 0.005f, 0.5f);
        changed |= ImGui::DragFloat("Extinction", &liquid.extinction, 0.5f, 0.0f, 200.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("大きいほど不透明 (血)、小さいほど透ける (水)。");
        changed |= ImGui::DragFloat("Specular", &liquid.specular, 0.01f, 0.0f, 4.0f);
        changed |= ImGui::DragFloat("Gloss", &liquid.gloss, 1.0f, 1.0f, 512.0f);
        changed |= ImGui::SliderFloat("Fresnel F0", &liquid.fresnelF0, 0.0f, 0.2f, "%.3f");
        ImGui::TreePop();
    }
    if (changed) MarkSettingsChanged();
}

void VolumeFlipbookBakePanel::DrawFramingWarnings()
{
    if (m_framing.boxCutFrames == 0 && m_framing.tileCutFrames == 0 && m_framing.overflowFrames == 0) return;
    if (m_framing.overflowFrames > 0) {
        ImGui::TextColored({ 1.0f, 0.45f, 0.3f, 1.0f }, "puff が多すぎます (最大 %u / 上限 %u): %s",
                           m_framing.maxLivePuffs, asset::kVolumeFillMaxPuffs,
                           DescribeFrameRange(m_framing.frameIssues, asset::kFramingTooManyPuffs).c_str());
        ImGui::TextDisabled("  上限を超えた分は描かれません。Count か Lifetime を下げてください。");
    }
    if (m_framing.boxCutFrames > 0) {
        ImGui::TextColored({ 1.0f, 0.45f, 0.3f, 1.0f }, "箱の面で煙が切れます: %s",
                           DescribeFrameRange(m_framing.frameIssues, asset::kFramingCutByVolumeBox).c_str());
        ImGui::TextDisabled("  プリセット・Start Time・コマ数を見直すか、Noise Amplitude を下げてください。");
    }
    if (m_framing.tileCutFrames > 0) {
        ImGui::TextColored({ 1.0f, 0.45f, 0.3f, 1.0f }, "タイルの縁で煙が切れます: %s",
                           DescribeFrameRange(m_framing.frameIssues, asset::kFramingCutByTileEdge).c_str());
        ImGui::TextDisabled("  Framing を大きくしてください (パーティクルにすると四角い板に見えます)。");
    }
}

void VolumeFlipbookBakePanel::DrawDisplayControls()
{
    static constexpr const char* kBackgrounds[] = { "Dark", "Light", "Checker" };
    static constexpr const char* kSizes[] = { "256", "384", "512", "Fit" };
    int background = static_cast<int>(m_background);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("Background", &background, kBackgrounds, IM_ARRAYSIZE(kBackgrounds))) {
        m_background = static_cast<asset::VolumePreviewBackground>(background);
        m_previewDirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::Combo("Size", &m_previewSize, kSizes, IM_ARRAYSIZE(kSizes));
}

void VolumeFlipbookBakePanel::DrawVolumeTab(EditorContext& ctx)
{
    const bool busy = m_baker.IsBusy();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(m_playing ? "Pause" : "Play", { 64.0f, 0.0f })) m_playing = !m_playing;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("Speed", &m_playSpeed, 0.01f, 0.05f, 4.0f, "%.2fx");
    const float duration = BakeDuration();
    if (ImGui::SliderFloat("Time", &m_previewTime, 0.0f, duration, "%.2f s")) {
        m_playing = false;
        m_previewDirty = true;
    }
    static constexpr const char* kViews[] = { "Color", "Alpha", "Motion" };
    int view = static_cast<int>(m_view);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("View", &view, kViews, IM_ARRAYSIZE(kViews))) {
        m_view = static_cast<VolumeView>(view);
        m_previewDirty = true;
    }
    ImGui::SameLine();
    DrawDisplayControls();
    ImGui::SameLine();
    ImGui::Checkbox("Light Arrow", &m_showLightArrow);
    ImGui::EndDisabled();

    const auto target = m_baker.PreviewTarget();
    if (!target.IsValid() || ctx.imguiRenderer == nullptr || ctx.resources == nullptr) {
        if (!m_baker.Result().message.empty() && !busy)
            ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "%s", m_baker.Result().message.c_str());
        return;
    }
    void* rawId = ctx.imguiRenderer->GetImTextureID(target, *ctx.resources, 0);
    if (rawId == nullptr) return;

    const float side = PreviewWidth(1.0f);
    const float u0 = m_view == VolumeView::Motion ? 0.5f : 0.0f;
    ImGui::Image(widgets::ToImTextureID(rawId), { side, side }, { u0, 0.0f }, { u0 + 0.5f, 1.0f });
    const ImVec2 imageMin = ImGui::GetItemRectMin();
    const ImVec2 imageMax = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const int frames = (std::max)(m_settings.source.frameCount, 1);
    const int frame = std::clamp(static_cast<int>(m_previewTime / m_settings.source.frameDt), 0, frames - 1);
    const std::uint8_t issues = frame < static_cast<int>(m_framing.frameIssues.size())
        ? m_framing.frameIssues[static_cast<std::size_t>(frame)] : 0;
    if (issues != 0 && !busy) {
        drawList->AddRect(imageMin, imageMax, kWarningColor, 0.0f, 0, 3.0f);
        const char* label = (issues & asset::kFramingCutByVolumeBox) ? "箱の面で切れる"
            : (issues & asset::kFramingCutByTileEdge)                ? "タイルの縁で切れる"
                                                                     : "puff が多すぎる";
        drawList->AddText({ imageMin.x + 6.0f, imageMax.y - 20.0f }, kWarningColor, label);
    }

    if (m_showLightArrow && m_view != VolumeView::Motion && !busy) {
        const asset::VolumeFlipbookCamera camera = asset::ComputeVolumeFlipbookCamera(m_settings);
        const float screenX = math::Vector3::Dot(camera.toLight, camera.right);
        const float screenY = -math::Vector3::Dot(camera.toLight, camera.up);
        const float depth = math::Vector3::Dot(camera.toLight, camera.forward);
        const ImVec2 center = { (imageMin.x + imageMax.x) * 0.5f, (imageMin.y + imageMax.y) * 0.5f };
        const ImVec2 tip = { center.x + screenX * side * 0.42f, center.y + screenY * side * 0.42f };
        drawList->AddLine(center, tip, kLightColor, 2.0f);
        drawList->AddCircleFilled(tip, 5.0f, kLightColor);
        // 奥から当たる光 (逆光) と手前から当たる光 (順光) は、画面上の矢印だけでは区別できない。
        const char* facing = depth > 0.3f ? "Light (逆光)" : depth < -0.3f ? "Light (順光)" : "Light";
        drawList->AddText({ tip.x + 8.0f, tip.y - 8.0f }, kLightColor, facing);
    }

    ImGui::TextDisabled("Frame %d / %d", frame, frames - 1);
    if (m_view == VolumeView::Motion)
        ImGui::TextDisabled("赤 = 右へ / 緑 = 下へ動く (灰色は静止)");
}

void VolumeFlipbookBakePanel::DrawFlipbookTab(EditorContext& ctx)
{
    if (!m_compare->HasFlipbook()) {
        ImGui::TextDisabled("Bake すると、ここで MV なし / MV ありを並べて再生できます。");
        return;
    }
    if (ImGui::Button(m_comparePlaying ? "Pause" : "Play", { 64.0f, 0.0f })) m_comparePlaying = !m_comparePlaying;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat("Playback FPS", &m_compareFps, 0.1f, 1.0f, 60.0f, "%.1f");
    ImGui::SameLine();
    if (ImGui::SmallButton("Baked FPS")) m_compareFps = 1.0f / m_compare->FrameDt();
    const int frames = m_compare->FrameCount();
    float framePosition = m_compareTime * m_compareFps;
    if (ImGui::SliderFloat("Frame", &framePosition, 0.0f, static_cast<float>((std::max)(frames - 1, 1)), "%.2f")) {
        m_comparePlaying = false;
        m_compareTime = framePosition / (std::max)(m_compareFps, 0.1f);
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("MV Strength", &m_strengthScale, 0.0f, 2.0f, "x%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("焼いたときの S に掛ける倍率。1 が正しい値です。\n"
                          "0 で MV なしと同じ、2 で行き過ぎ (輪郭が逆向きに流れる) になります。");
    ImGui::SameLine();
    DrawDisplayControls();

    if (ctx.imguiRenderer == nullptr || ctx.resources == nullptr) return;
    void* rawId = ctx.imguiRenderer->GetImTextureID(m_compare->Target(), *ctx.resources, 0);
    if (rawId == nullptr) return;
    const float width = PreviewWidth(2.0f);
    ImGui::Image(widgets::ToImTextureID(rawId), { width, width * 0.5f });
    const ImVec2 imageMin = ImGui::GetItemRectMin();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddText({ imageMin.x + 6.0f, imageMin.y + 4.0f }, IM_COL32(235, 235, 235, 255), "MV なし");
    drawList->AddText({ imageMin.x + width * 0.5f + 6.0f, imageMin.y + 4.0f }, IM_COL32(235, 235, 235, 255), "MV あり");
    drawList->AddLine({ imageMin.x + width * 0.5f, imageMin.y }, { imageMin.x + width * 0.5f, imageMin.y + width * 0.5f },
                      IM_COL32(20, 20, 20, 255), 2.0f);

    ImGui::TextDisabled("Frame %d → 次のコマ  blend %.2f   S = %.4f%s", m_compare->CurrentFrame(),
                        m_compare->CurrentBlend(), m_compare->MotionStrength(), m_compare->Loops() ? "   (Loop)" : "");
    ImGui::TextDisabled("Playback FPS を下げるほど、コマ間の補間の差がよく見えます。");
}

void VolumeFlipbookBakePanel::StartBake(EditorContext& ctx)
{
    if (ctx.resources == nullptr) return;
    asset::VolumeFlipbookBakeSettings settings = m_settings;
    settings.outputDirectory = GeneratedDirectory(ctx.projectRoot, "Assets/Textures/Generated/VFX");
    settings.baseName = std::string(m_baseName.data()) + "_" + settings.source.preset + "_"
        + std::to_string(settings.source.seed);

    std::string error;
    if (!m_baker.Begin(settings, *ctx.resources, error)) {
        m_status = error;
        m_statusIsError = true;
        return;
    }
    m_playing = false;
    m_resultHandled = false;
    m_status.clear();
}

void VolumeFlipbookBakePanel::DrawBake(EditorContext& ctx)
{
    ImGui::SeparatorText("Bake");
    if (m_baker.IsBusy()) {
        const int done = m_baker.CompletedFrames();
        const int total = (std::max)(m_baker.TotalFrames(), 1);
        char overlay[32]{};
        std::snprintf(overlay, sizeof(overlay), "%d / %d", done, total);
        ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), { -1.0f, 0.0f }, overlay);
        if (ImGui::Button("Cancel", { -1.0f, 0.0f })) m_baker.Cancel();
        return;
    }
    if (ImGui::Button("Bake", { -1.0f, 0.0f })) StartBake(ctx);
    if (m_framing.boxCutFrames > 0 || m_framing.tileCutFrames > 0 || m_framing.overflowFrames > 0)
        ImGui::TextDisabled("構図の警告が出ています。このまま焼くと欠けたコマが入ります。");
    if (!m_status.empty()) {
        if (m_statusIsError)
            ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "%s", m_status.c_str());
        else
            ImGui::TextWrapped("%s", m_status.c_str());
    }
    if (m_lastResult.success) {
        ImGui::TextDisabled("%s", NormalizeAssetPath(m_lastResult.colorPath).c_str());
        ImGui::TextDisabled("%s", NormalizeAssetPath(m_lastResult.motionPath).c_str());
    }
}

void VolumeFlipbookBakePanel::DrawApply(EditorContext& ctx)
{
    ImGui::SeparatorText("Apply to Material");
    widgets::AssetPathField("Particle Material", m_materialPath, ".mat", ctx.projectRoot);
    const bool busy = m_baker.IsBusy();
    ImGui::BeginDisabled(!m_lastResult.success || m_materialPath.empty() || busy);
    if (ImGui::Button("Apply", { -1.0f, 0.0f })) ApplyToMaterial(ctx);
    ImGui::EndDisabled();
    ImGui::TextDisabled("albedo / tex5 / コマ割り / Frame Blending / Motion Strength /\n"
                        "Premultiplied / HDR Emissive をまとめて設定します。");

    ImGui::BeginDisabled(m_materialPath.empty() || busy || ctx.InPrefabEditMode());
    if (ImGui::Button("Preview as VFX", { -1.0f, 0.0f })) OpenInPrefabPreview(ctx);
    ImGui::EndDisabled();
    if (ctx.InPrefabEditMode())
        ImGui::TextDisabled("Prefab 編集中は開けません (先に閉じてください)。");
    else
        ImGui::TextDisabled("この .mat を使うエミッター 1 つだけの .vfx を作り、Prefab 編集モードで再生します。\n"
                            "今のシーンは一時退避され、Prefab を閉じると戻ります。");
}

void VolumeFlipbookBakePanel::ApplyToMaterial(EditorContext& ctx)
{
    const std::string file = asset::AssetManager::ResolveAssetPath(m_materialPath);
    asset::MaterialAsset oldAsset;
    if (file.empty() || !asset::LoadMaterialAssetFromFile(file, oldAsset)) {
        m_status = "マテリアルを読み込めません: " + m_materialPath;
        m_statusIsError = true;
        return;
    }

    const asset::VolumeFlipbookBakeResult& result = m_lastResult;
    asset::MaterialAsset newAsset = oldAsset;
    newAsset.textures["albedo"] = NormalizeAssetPath(result.colorPath);
    newAsset.textures["tex5"] = NormalizeAssetPath(result.motionPath);
    newAsset.blendMode = renderer::BlendMode::PREMULTIPLIED;
    auto& particle = newAsset.particle;
    particle.alphaSource = scene::ParticleAlphaSource::TextureAlpha;
    particle.spriteColumns = result.columns;
    particle.spriteRows = result.rows;
    particle.spriteStartFrame = 0;
    particle.spriteEndFrame = result.frameCount - 1;
    particle.spriteRandomRow = false;
    particle.spriteRandomStartFrame = false;
    // MV は spriteBlend が 0 だと一切効かない。
    particle.flipbookFrameBlending = true;
    particle.motionVectorFlipbook = true;
    particle.motionVectorStrength = result.recommendedStrength;
    particle.emissiveScale = result.suggestedEmissiveScale;
    particle.distortion = false;
    if (asset::VolumeSourceLoops(m_settings.source)) {
        particle.flipbookMode = scene::ParticleFlipbookMode::FramesPerSecond;
        particle.flipbookFramesPerSecond = 1.0f / m_settings.source.frameDt;
    } else {
        particle.flipbookMode = scene::ParticleFlipbookMode::Lifetime;
    }

    EditorContext* context = &ctx;
    const auto applyToDisk = [context, file](const asset::MaterialAsset& value) {
        if (!asset::SaveMaterialAssetToFile(file, value)) return;
        asset::AssetManager::ReloadPath(file);
        context->requestAssetBrowserRefresh = true;
    };
    auto command = std::make_unique<LambdaCommand>("Apply Volume Flipbook",
        [applyToDisk, newAsset]() { applyToDisk(newAsset); },
        [applyToDisk, oldAsset]() { applyToDisk(oldAsset); });
    if (ctx.undoStack != nullptr)
        ctx.undoStack->Execute(std::move(command));
    else
        command->Execute();
    m_status = "適用しました: " + m_materialPath;
    m_statusIsError = false;
}

void VolumeFlipbookBakePanel::OpenInPrefabPreview(EditorContext& ctx)
{
    const std::string materialPath = NormalizeAssetPath(m_materialPath);
    const std::string stem = std::filesystem::path(materialPath).stem().string();
    const std::filesystem::path directory = GeneratedDirectory(ctx.projectRoot, "Assets/VFX/Generated");
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    const std::filesystem::path file = directory / (stem + "_Preview.vfx");

    // 1 粒の寿命でアトラスを 1 周させる。焼いた長さそのままにすると、焼いたときの速さで動く。
    const int frames = m_lastResult.success ? m_lastResult.frameCount : m_settings.source.frameCount;
    const float lifetime = (std::max)(static_cast<float>(frames) * m_settings.source.frameDt, 0.1f);
    if (directoryError || !WritePreviewVfx(file, stem + "_Preview", materialPath, lifetime)) {
        m_status = "プレビュー用の .vfx を書き出せません: " + file.string();
        m_statusIsError = true;
        return;
    }
    ctx.requestAssetBrowserRefresh = true;

    OpArgs args;
    args.Set("path", file.generic_string());
    const OpResult result = InvokeOperator(ctx, "asset.open", args);
    m_status = result.ok ? "Prefab 編集モードで開きます: " + NormalizeAssetPath(file.generic_string())
                         : "開けません: " + result.message;
    m_statusIsError = !result.ok;
}

} // namespace fbzz::editor
