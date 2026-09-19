/// @file    SfxEditorPanel.cpp
/// @brief   .synth の編集・プレビュー・書き出し UI。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Editor/Panels/SfxEditorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Asset/SynthAsset.hpp>
#include <Engine/Audio/Synth.hpp>
#include <Engine/Core/Logger.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace fbzz::editor {
namespace {

constexpr int kWaveformResolution = 512;

const char* const kWaveLabels[] = { "Sine", "Square", "Saw", "Triangle", "Noise" };

/// パネルのボタンはすべて Operator を呼ぶ。人と AI で実装を分けない。
void Invoke(EditorContext& ctx, const char* id, const OpArgs& args = {})
{
    const OpResult result = InvokeOperator(ctx, id, args);
    if (!result.ok)
        FBZZ_LOG_WARN("SFX Editor: %s failed (%s) %s",
                      id, result.errorCode.c_str(), result.message.c_str());
}

bool Slider(const char* label, float& value, float min, float max, const char* tooltip = nullptr)
{
    const bool changed = ImGui::SliderFloat(label, &value, min, max);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return changed;
}

} // namespace

void SfxEditorPanel::EnsureWaveform(const EditorContext& ctx)
{
    const uint64_t hash = audio::HashSpec(ctx.sfxEditorSpec);
    if (m_waveformValid && hash == m_waveformSpecHash) return;
    m_waveformValid    = true;
    m_waveformSpecHash = hash;
    m_waveform.clear();

    audio::WaveFormat fmt{};
    const std::vector<uint8_t> pcm = audio::Render(ctx.sfxEditorSpec, fmt);
    m_sampleCount = pcm.size() / 2;
    m_durationSeconds = fmt.sampleRate > 0
        ? static_cast<float>(m_sampleCount) / static_cast<float>(fmt.sampleRate) : 0.0f;
    if (m_sampleCount == 0) return;

    /// @note 16bit リトルエンディアンをバイトから組み立てる (Synth::Render と対)。
    const auto sampleAt = [&pcm](size_t index) {
        const auto lo = static_cast<uint16_t>(pcm[index * 2]);
        const auto hi = static_cast<uint16_t>(pcm[index * 2 + 1]);
        return static_cast<int>(static_cast<int16_t>(static_cast<uint16_t>(lo | (hi << 8))));
    };

    /// @note 最大値で間引く: 平均を取ると高い周波数の波形が打ち消し合ってほぼ直線に見え、
    ///       「音が出ているのか」すら読めなくなる。
    const size_t buckets = (std::min)(static_cast<size_t>(kWaveformResolution), m_sampleCount);
    m_waveform.resize(buckets);
    for (size_t i = 0; i < buckets; ++i) {
        const size_t begin = i * m_sampleCount / buckets;
        const size_t end   = (std::max)(begin + 1, (i + 1) * m_sampleCount / buckets);
        int peak = 0;
        for (size_t s = begin; s < end && s < m_sampleCount; ++s) {
            const int value = sampleAt(s);
            if (std::abs(value) > std::abs(peak)) peak = value;
        }
        m_waveform[i] = static_cast<float>(peak) / 32768.0f;
    }
}

void SfxEditorPanel::LoadRequested(EditorContext& ctx)
{
    if (m_requestedPath.empty()) return;
    const std::string path = m_requestedPath;
    m_requestedPath.clear();

    asset::SynthAsset loaded;
    if (!asset::LoadSynthAssetFromFile(path, loaded)) {
        FBZZ_LOG_WARN("SFX Editor: cannot open [%s]", path.c_str());
        return;
    }
    /// @note 未保存の下書きは開いた時点で失われる (Behavior Tree / Animation Graph と同じ)。
    ///       黙って消えると原因が追えないので、せめて Console には残す。
    if (ctx.sfxEditorDirty) {
        FBZZ_LOG_WARN("SFX Editor: 未保存の変更を破棄して [%s] を開きます", path.c_str());
    }
    ctx.sfxEditorSpec       = loaded.spec;
    ctx.sfxEditorPresetName = loaded.presetName;
    ctx.sfxEditorPath       = path;
    ctx.sfxEditorDirty      = false;
    m_waveformValid         = false;
}

bool SfxEditorPanel::DrawParameters(EditorContext& ctx)
{
    audio::SynthSpec& spec = ctx.sfxEditorSpec;
    bool changed = false;

    int wave = static_cast<int>(spec.wave);
    if (ImGui::Combo("Wave", &wave, kWaveLabels, IM_ARRAYSIZE(kWaveLabels))) {
        OpArgs args;
        args.Set("wave", std::string(audio::WaveToString(static_cast<audio::SynthWave>(wave))));
        Invoke(ctx, "sfx.set_wave", args);
        changed = true;
    }

    /// @note スライダーは Operator を通さない: ドラッグ中は毎フレーム値が変わり、1 フレームごとに
    ///       sfx.set_param を呼ぶと AI 向けの粒度の操作が秒間 60 回走る。実体は同じ
    ///       ctx.sfxEditorSpec なので直接書き、離した時点の状態を Operator 経由と揃える。
    if (ImGui::CollapsingHeader("Envelope", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= Slider("Attack",  spec.attack,  0.0f, 1.0f, "立ち上がりにかける秒数");
        changed |= Slider("Sustain", spec.sustain, 0.0f, 2.0f, "最大音量を保つ秒数");
        changed |= Slider("Decay",   spec.decay,   0.0f, 2.0f, "消えるまでの秒数");
        changed |= Slider("Punch",   spec.punch,   0.0f, 1.0f,
                          "発音直後だけ音量を持ち上げる。打撃感を足す");
    }

    if (ImGui::CollapsingHeader("Pitch", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::DragFloat("Start Frequency", &spec.startFrequency,
                                    2.0f, 20.0f, 18000.0f, "%.0f Hz");
        changed |= ImGui::DragFloat("Min Frequency", &spec.minFrequency,
                                    2.0f, 0.0f, 18000.0f, "%.0f Hz");
        changed |= Slider("Slide", spec.slide, -16.0f, 16.0f,
                          "1 秒あたりのオクターブ変化。負で下降");
        changed |= Slider("Vibrato Depth", spec.vibratoDepth, 0.0f, 1.0f);
        changed |= Slider("Vibrato Rate",  spec.vibratoRate,  0.0f, 60.0f);
    }

    if (ImGui::CollapsingHeader("Timbre", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(spec.wave != audio::SynthWave::Square);
        changed |= Slider("Duty Cycle", spec.dutyCycle, 0.01f, 0.99f, "Square 波のみ有効");
        changed |= Slider("Duty Sweep", spec.dutySweep, -4.0f, 4.0f);
        ImGui::EndDisabled();
        changed |= Slider("Low Pass",       spec.lowPassCutoff,  0.0f, 1.0f, "1 で無加工");
        changed |= Slider("Low Pass Sweep", spec.lowPassSweep,  -4.0f, 4.0f);
        changed |= Slider("High Pass",      spec.highPassCutoff, 0.0f, 1.0f, "0 で無加工");
        changed |= Slider("Bit Crush",      spec.bitCrush,       0.0f, 1.0f,
                          "量子化を粗くしてレトロな質感にする");
    }

    if (ImGui::CollapsingHeader("Repeat")) {
        changed |= Slider("Repeat Rate",  spec.repeatRate,  0.0f, 60.0f,
                          "発振器を作り直す頻度。0 で反復なし");
        changed |= Slider("Arpeggio Mod", spec.arpeggioMod, 0.0f, 3.0f,
                          "反復ごとに周波数へ掛ける倍率");
    }

    if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= Slider("Amplitude", spec.amplitude, 0.0f, 1.0f);
        int seed = static_cast<int>(spec.seed);
        if (ImGui::DragInt("Seed", &seed, 1.0f, 0, 999999)) {
            spec.seed = static_cast<uint32_t>((std::max)(seed, 0));
            changed = true;
        }
    }

    return changed;
}

void SfxEditorPanel::OnRenderContent(EditorContext& ctx)
{
    LoadRequested(ctx);

    if (ctx.sfxEditorPath.empty()) {
        ImGui::TextDisabled("(未保存の下書き — Save で保存先を決める)");
    } else {
        std::string shown = ctx.sfxEditorPath;
        if (!ctx.projectRoot.empty() && shown.rfind(ctx.projectRoot, 0) == 0)
            shown = shown.substr(ctx.projectRoot.size() + 1);
        ImGui::Text("%s%s", shown.c_str(), ctx.sfxEditorDirty ? " *" : "");
    }

    ImGui::BeginDisabled(!CanInvokeOperator(ctx, "sfx.save"));
    if (ImGui::Button("Save")) Invoke(ctx, "sfx.save");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!CanInvokeOperator(ctx, "sfx.export_wav"));
    if (ImGui::Button("Export .wav")) Invoke(ctx, "sfx.export_wav");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && ctx.sfxEditorPath.empty())
        ImGui::SetTooltip("先に .synth を保存してください");

    ImGui::Separator();

    ImGui::TextUnformatted("Preset");
    for (int i = 0; i < static_cast<int>(audio::SynthPreset::Count); ++i) {
        if (i > 0) ImGui::SameLine();
        const char* name = audio::PresetToString(static_cast<audio::SynthPreset>(i));
        if (ImGui::SmallButton(name)) {
            OpArgs args;
            args.Set("preset", std::string(name));
            Invoke(ctx, "sfx.load_preset", args);
            if (m_autoPreview) Invoke(ctx, "sfx.preview");
        }
    }

    if (ImGui::Button("Randomize")) {
        Invoke(ctx, "sfx.randomize");
        if (m_autoPreview) Invoke(ctx, "sfx.preview");
    }
    ImGui::SameLine();
    if (ImGui::Button("Mutate")) {
        Invoke(ctx, "sfx.mutate");
        if (m_autoPreview) Invoke(ctx, "sfx.preview");
    }
    ImGui::SameLine();
    if (ImGui::Button("Play")) Invoke(ctx, "sfx.preview");
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &m_autoPreview);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("値を変えるたびに自動で鳴らす");

    EnsureWaveform(ctx);
    if (!m_waveform.empty()) {
        ImGui::PlotLines("##waveform", m_waveform.data(),
                         static_cast<int>(m_waveform.size()), 0, nullptr,
                         -1.0f, 1.0f, ImVec2(-1.0f, 70.0f));
        ImGui::TextDisabled("%.3f s  /  %zu samples", m_durationSeconds, m_sampleCount);
    } else {
        ImGui::TextDisabled("(無音 — Sustain と Decay が 0、または Amplitude が 0)");
    }

    ImGui::Separator();

    if (DrawParameters(ctx)) {
        ctx.sfxEditorDirty = true;
        m_previewPending   = true;
    }
    /// @note 操作を離してから鳴らす: ドラッグ中に毎フレーム鳴らすと同じ音が何十も重なる。
    if (m_previewPending && !ImGui::IsAnyItemActive()) {
        m_previewPending = false;
        if (m_autoPreview) Invoke(ctx, "sfx.preview");
    }
}

} // namespace fbzz::editor
