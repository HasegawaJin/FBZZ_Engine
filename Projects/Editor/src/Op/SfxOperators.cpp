/// @file    SfxOperators.cpp
/// @brief   手続き効果音 (.synth) の Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// WHY 専用の MCP ツールではなく Operator にするか:
///   editor.op.list / invoke / query は登録簿を総当たりで公開するので、ここへ
///   足した操作はそのまま AI から呼べる。専用ツールを起こすと contracts /
///   tools.ts / C++ ハンドラの 3 箇所へ同じ意味を書くことになり、
///   人の面 (パレット) には出ないまま層が増える。
///   必須引数を持つ操作はコマンドパレットが自動で除外するため、
///   sfx.set_param のような AI 向けの粒度を登録しても人の面は汚れない
///   (EditorApp_CommandPalette.cpp の singleEnumParam / hasRequiredParam 判定)。
///
/// WHY 対象を「編集中の下書き」に固定するか:
///   Behavior Tree / Animation Graph と同じ理由。パスを引数に取ってファイルへ
///   直接書くと、SFX Editor が抱えている編集中の値と 2 つの真実ができる。
///   編集中の実体は EditorContext::sfxEditorSpec ただ 1 つで、パネルはそれを描く面。
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/SynthAsset.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Audio/Synth.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

// set_param が受け付ける名前。SynthSpec の float フィールドと 1:1 で対応する。
// WHY 表にするか: enumValues として宣言すれば op.list が候補をそのまま返し、
//     AI が綴りを推測しなくて済む。範囲も同じ表から引くので、
//     「設定はできるのに検問だけ別の値」という食い違いが起きない。
struct SpecField {
    const char* name;
    float       min;
    float       max;
    float audio::SynthSpec::* member;
};

constexpr SpecField kSpecFields[] = {
    { "attack",          0.0f,     5.0f,  &audio::SynthSpec::attack },
    { "sustain",         0.0f,     5.0f,  &audio::SynthSpec::sustain },
    { "decay",           0.0f,     5.0f,  &audio::SynthSpec::decay },
    { "punch",           0.0f,     2.0f,  &audio::SynthSpec::punch },
    { "startFrequency", 20.0f, 18000.0f,  &audio::SynthSpec::startFrequency },
    { "minFrequency",    0.0f, 18000.0f,  &audio::SynthSpec::minFrequency },
    { "slide",         -32.0f,    32.0f,  &audio::SynthSpec::slide },
    { "vibratoDepth",    0.0f,     1.0f,  &audio::SynthSpec::vibratoDepth },
    { "vibratoRate",     0.0f,   200.0f,  &audio::SynthSpec::vibratoRate },
    { "dutyCycle",       0.01f,    0.99f, &audio::SynthSpec::dutyCycle },
    { "dutySweep",      -8.0f,     8.0f,  &audio::SynthSpec::dutySweep },
    { "lowPassCutoff",   0.0f,     1.0f,  &audio::SynthSpec::lowPassCutoff },
    { "lowPassSweep",   -8.0f,     8.0f,  &audio::SynthSpec::lowPassSweep },
    { "highPassCutoff",  0.0f,     1.0f,  &audio::SynthSpec::highPassCutoff },
    { "bitCrush",        0.0f,     1.0f,  &audio::SynthSpec::bitCrush },
    { "repeatRate",      0.0f,   200.0f,  &audio::SynthSpec::repeatRate },
    { "arpeggioMod",     0.0f,     8.0f,  &audio::SynthSpec::arpeggioMod },
    { "amplitude",       0.0f,     1.0f,  &audio::SynthSpec::amplitude },
};

std::vector<std::string> SpecFieldNames()
{
    std::vector<std::string> names;
    names.reserve(std::size(kSpecFields));
    for (const SpecField& field : kSpecFields) names.emplace_back(field.name);
    return names;
}

std::vector<std::string> PresetNames()
{
    std::vector<std::string> names;
    for (int i = 0; i < static_cast<int>(audio::SynthPreset::Count); ++i)
        names.emplace_back(audio::PresetToString(static_cast<audio::SynthPreset>(i)));
    return names;
}

const SpecField* FindSpecField(const std::string& name)
{
    for (const SpecField& field : kSpecFields)
        if (name == field.name) return &field;
    return nullptr;
}

OpData SpecToData(const audio::SynthSpec& spec)
{
    OpData data = OpData::MakeObject();
    data.Set("wave", std::string(audio::WaveToString(spec.wave)));
    for (const SpecField& field : kSpecFields)
        data.Set(field.name, spec.*(field.member));
    data.Set("seed", static_cast<int>(spec.seed));
    return data;
}

OpData AnalysisToData(const audio::SynthAnalysis& analysis)
{
    OpData data = OpData::MakeObject();
    data.Set("durationSeconds",   analysis.durationSeconds);
    data.Set("peakAmplitude",     analysis.peakAmplitude);
    data.Set("rmsAmplitude",      analysis.rmsAmplitude);
    data.Set("brightnessHz",      analysis.brightnessHz);
    data.Set("zeroCrossingHz",    analysis.zeroCrossingHz);
    data.Set("timeToPeakSeconds", analysis.timeToPeakSeconds);
    data.Set("clippedRatio",      analysis.clippedRatio);
    data.Set("sampleRate",        static_cast<int>(analysis.sampleRate));
    data.Set("sampleCount",       static_cast<int>(analysis.sampleCount));
    return data;
}

void PreviewSpec(const audio::SynthSpec& spec)
{
    auto* manager = core::Application::Get().GetAudioManager();
    if (!manager) return;
    const auto clip = manager->AcquireGeneratedClip(spec);
    if (clip == 0) return;
    (void)manager->PlayClipVoice(clip, false, manager->FindBus("UI"));
    // 再生中は voice が実体を押さえる。Application::Update が終了後に回収する。
    manager->ReleaseClip(clip);
}

// "Assets/..." もプロジェクト外の絶対パスも受け取れるようにする。
std::string ResolveSavePath(const EditorContext& ctx, const std::string& path)
{
    if (path.empty()) return {};
    const bool absolute = (path.size() > 1 && path[1] == ':') || path[0] == '/';
    if (absolute || ctx.projectRoot.empty()) return path;
    return ctx.projectRoot + "/" + path;
}

// 呼ぶたびに違う結果が欲しい探索操作 (Randomize / Mutate) の既定シード。
uint32_t NextExplorationSeed()
{
    static uint32_t seed = 1;
    return seed++;
}

OpParam MakeSeedParam()
{
    OpParam param;
    param.name     = "seed";
    param.type     = OpParamType::Int;
    param.desc     = "0 または省略で、呼ぶたびに違う結果になる";
    param.required = false;
    param.defaultValue = OpValue{ 0 };
    param.hasRange = true;
    param.minValue = 0.0f;
    param.maxValue = 999999.0f;
    return param;
}

} // namespace

void RegisterSfxOperators(OperatorRegistry& registry)
{
    // 下書きは常に存在する (既定構築の SynthSpec)。「開いていないから使えない」を
    // 作らないことで、AI はパネルを開かずに音を作って保存まで到達できる。
    const auto hasSavePath = [](const OpContext& c, const OpArgs& args) {
        return !args.GetString("path").empty() || !c.ctx.sfxEditorPath.empty();
    };

    {
        EditorOperator op;
        op.id       = "sfx.load_preset";
        op.label    = "Load SFX Preset";
        op.category = "SFX";
        op.desc     = "編集中の手続き効果音を、指定プリセットの基準パラメーターで置き換える。";
        op.caution  = "編集中の未保存の値は失われる。";
        op.kind     = OpKind::Action;

        OpParam presetParam;
        presetParam.name       = "preset";
        presetParam.type       = OpParamType::String;
        presetParam.desc       = "プリセット名";
        presetParam.enumValues = PresetNames();
        op.params = { presetParam, MakeSeedParam() };

        op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
            const std::string name = args.GetString("preset");
            const auto preset = audio::PresetFromString(name.c_str(), audio::SynthPreset::Blip);
            c.ctx.sfxEditorSpec =
                audio::MakePreset(preset, static_cast<uint32_t>(args.GetInt("seed", 0)));
            c.ctx.sfxEditorPresetName = audio::PresetToString(preset);
            c.ctx.sfxEditorDirty      = true;
            return OpResult::Data(SpecToData(c.ctx.sfxEditorSpec));
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "sfx.set_wave";
        op.label    = "Set SFX Wave";
        op.category = "SFX";
        op.desc     = "基本波形を変える。Noise では startFrequency が「粗さ」として効く。";
        op.kind     = OpKind::Action;

        OpParam waveParam;
        waveParam.name       = "wave";
        waveParam.type       = OpParamType::String;
        waveParam.desc       = "波形名";
        waveParam.enumValues = { "sine", "square", "saw", "triangle", "noise" };
        op.params = { waveParam };

        op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
            const std::string name = args.GetString("wave");
            c.ctx.sfxEditorSpec.wave =
                audio::WaveFromString(name.c_str(), c.ctx.sfxEditorSpec.wave);
            c.ctx.sfxEditorPresetName.clear();
            c.ctx.sfxEditorDirty = true;
            return OpResult::Data(SpecToData(c.ctx.sfxEditorSpec));
        };
        op.checked = [](const OpContext& c, const OpArgs& args) {
            if (!args.Has("wave")) return false;
            return args.GetString("wave") == audio::WaveToString(c.ctx.sfxEditorSpec.wave);
        };
        registry.Register(std::move(op));
    }

    {
        // 必須引数が 2 つあるためコマンドパレットには出ない (AI 向けの粒度)。
        EditorOperator op;
        op.id       = "sfx.set_param";
        op.label    = "Set SFX Parameter";
        op.category = "SFX";
        op.desc     = "編集中の手続き効果音のパラメーターを 1 つ書き換え、"
                      "書き換え後の全パラメーターを返す。読み直す往復が要らない。";
        op.kind     = OpKind::Action;

        OpParam nameParam;
        nameParam.name       = "name";
        nameParam.type       = OpParamType::String;
        nameParam.desc       = "パラメーター名";
        nameParam.enumValues = SpecFieldNames();

        // WHY value に range を宣言しないか: 許容範囲は name ごとに違う。
        //     1 つの宣言では表せないので、exec 側で表を引いて弾く。
        OpParam valueParam;
        valueParam.name = "value";
        valueParam.type = OpParamType::Float;
        valueParam.desc = "値。許容範囲は name ごとに異なり、外すと BAD_ARG になる";
        op.params = { nameParam, valueParam };

        op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
            const std::string name = args.GetString("name");
            const SpecField* field = FindSpecField(name);
            if (!field)
                return OpResult::Err("BAD_ARG", "未知のパラメーター: " + name);

            const float value = args.GetFloat("value");
            if (value < field->min || value > field->max) {
                // WHY クランプせず弾くか: 黙って丸めると「受理されたのに指定値と違う」
                //     という食い違いになり、AI は次の一手を誤った前提で決める。
                return OpResult::Err(
                    "BAD_ARG", name + " の範囲は " + std::to_string(field->min) + " 〜 "
                                   + std::to_string(field->max));
            }
            c.ctx.sfxEditorSpec.*(field->member) = value;
            c.ctx.sfxEditorPresetName.clear();
            c.ctx.sfxEditorDirty = true;
            return OpResult::Data(SpecToData(c.ctx.sfxEditorSpec));
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "sfx.randomize";
        op.label    = "Randomize SFX";
        op.category = "SFX";
        op.desc     = "全パラメーターをランダムに決め直す。当たりを引くための探索用。";
        op.caution  = "編集中の未保存の値は失われる。";
        op.kind     = OpKind::Action;
        op.params   = { MakeSeedParam() };

        op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
            const auto requested = static_cast<uint32_t>(args.GetInt("seed", 0));
            c.ctx.sfxEditorSpec =
                audio::Randomize(requested != 0 ? requested : NextExplorationSeed());
            c.ctx.sfxEditorPresetName.clear();
            c.ctx.sfxEditorDirty = true;
            return OpResult::Data(SpecToData(c.ctx.sfxEditorSpec));
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "sfx.mutate";
        op.label    = "Mutate SFX";
        op.category = "SFX";
        op.desc     = "現在の値を少しだけ揺らした派生を作る。"
                      "同じ系統のバリエーションが欲しいときはこちら。";
        op.kind     = OpKind::Action;

        OpParam amountParam;
        amountParam.name         = "amount";
        amountParam.type         = OpParamType::Float;
        amountParam.desc         = "揺らし幅 0-1";
        amountParam.required     = false;
        amountParam.defaultValue = OpValue{ 0.15f };
        amountParam.hasRange     = true;
        amountParam.minValue     = 0.0f;
        amountParam.maxValue     = 1.0f;
        op.params = { amountParam, MakeSeedParam() };

        op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
            const auto requested = static_cast<uint32_t>(args.GetInt("seed", 0));
            c.ctx.sfxEditorSpec = audio::Mutate(
                c.ctx.sfxEditorSpec, args.GetFloat("amount", 0.15f),
                requested != 0 ? requested : NextExplorationSeed());
            c.ctx.sfxEditorDirty = true;
            return OpResult::Data(SpecToData(c.ctx.sfxEditorSpec));
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "sfx.preview";
        op.label    = "Preview SFX";
        op.category = "SFX";
        op.desc     = "編集中の音を 1 度鳴らす。Play モードでなくても鳴る。";
        op.kind     = OpKind::Action;
        op.exec     = [](OpContext& c, const OpArgs&) -> OpResult {
            if (!core::Application::Get().GetAudioManager())
                return OpResult::Err("NO_AUDIO", "オーディオデバイスが初期化されていません");
            PreviewSpec(c.ctx.sfxEditorSpec);
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }

    {
        // Query。パレットには出ず、AI の read 権限で呼べる。
        EditorOperator op;
        op.id       = "sfx.inspect";
        op.label    = "Inspect SFX";
        op.category = "SFX";
        op.desc     = "編集中 (または path 指定の .synth) のパラメーターと、合成結果の"
                      "特徴量を返す。AI は音を聴けないため、brightnessHz (明るさ) / "
                      "rmsAmplitude (体感音量) / timeToPeakSeconds (打撃感) / "
                      "zeroCrossingHz (ノイズ性) を手がかりに調整する。"
                      "clippedRatio が 0 より大きいなら amplitude が過大。";
        op.kind     = OpKind::Query;

        OpParam pathParam;
        pathParam.name     = "path";
        pathParam.type     = OpParamType::String;
        pathParam.desc     = "省略時は編集中の下書き";
        pathParam.required = false;
        op.params = { pathParam };

        op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
            audio::SynthSpec spec   = c.ctx.sfxEditorSpec;
            std::string      source = c.ctx.sfxEditorPath.empty()
                ? std::string("(draft)") : c.ctx.sfxEditorPath;
            std::string      preset = c.ctx.sfxEditorPresetName;
            bool             dirty  = c.ctx.sfxEditorDirty;

            if (const std::string path = args.GetString("path"); !path.empty()) {
                asset::SynthAsset loaded;
                const std::string resolved = asset::AssetManager::ResolveAssetPath(path);
                if (!asset::LoadSynthAssetFromFile(resolved.empty() ? path : resolved, loaded))
                    return OpResult::Err("ASSET_NOT_FOUND", ".synth を読めません: " + path);
                spec   = loaded.spec;
                source = path;
                preset = loaded.presetName;
                dirty  = false;
            }

            OpData data = OpData::MakeObject();
            data.Set("source", source);
            data.Set("preset", preset.empty() ? std::string("(custom)") : preset);
            data.Set("dirty", dirty);
            data.Set("spec", SpecToData(spec));
            data.Set("analysis", AnalysisToData(audio::Analyze(spec)));
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "sfx.save";
        op.label    = "Save SFX";
        op.category = "SFX";
        op.desc     = "編集中の音を .synth へ保存する。path を渡すと保存先を変え、"
                      "以後の編集対象もそちらへ移る。";
        op.kind     = OpKind::Action;   // ファイル I/O。Undo には載せない

        OpParam pathParam;
        pathParam.name     = "path";
        pathParam.type     = OpParamType::String;
        pathParam.desc     = "保存先 (\"Assets/Sounds/laser.synth\")。省略時は現在の編集対象";
        pathParam.required = false;
        op.params = { pathParam };

        op.poll = hasSavePath;
        op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
            const std::string requested = args.GetString("path");
            const std::string target = requested.empty()
                ? c.ctx.sfxEditorPath : ResolveSavePath(c.ctx, requested);
            if (target.empty())
                return OpResult::Err("NO_PATH", "保存先が決まっていません");

            asset::SynthAsset asset;
            asset.spec       = c.ctx.sfxEditorSpec;
            asset.presetName = c.ctx.sfxEditorPresetName;
            if (!asset::SaveSynthAssetToFile(target, asset))
                return OpResult::Err("WRITE_FAILED", "保存できません: " + target);

            c.ctx.sfxEditorPath  = target;
            c.ctx.sfxEditorDirty = false;

            OpData data = OpData::MakeObject();
            data.Set("path", target);
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "sfx.export_wav";
        op.label    = "Export SFX to .wav";
        op.category = "SFX";
        op.desc     = "編集中の音を .wav へ焼き出す。保存先は .synth と同じ場所・同じ名前。";
        op.kind     = OpKind::Action;
        op.poll     = hasSavePath;
        op.exec     = [](OpContext& c, const OpArgs&) -> OpResult {
            if (c.ctx.sfxEditorPath.empty())
                return OpResult::Err("NO_PATH", "先に .synth を保存してください");

            audio::WaveFormat fmt{};
            const std::vector<uint8_t> pcm = audio::Render(c.ctx.sfxEditorSpec, fmt);
            const std::vector<uint8_t> wav = audio::EncodeWav(pcm.data(), pcm.size(), fmt);
            if (wav.empty())
                return OpResult::Err("EMPTY", "無音のため書き出すものがありません");

            const std::string target =
                c.ctx.sfxEditorPath.substr(0, c.ctx.sfxEditorPath.rfind('.')) + ".wav";
            if (!util::FileSystem::WriteBinary(util::FileSystem::PathFromUtf8(target),
                                               wav.data(), wav.size()))
                return OpResult::Err("WRITE_FAILED", "書き出せません: " + target);

            OpData data = OpData::MakeObject();
            data.Set("path", target);
            data.Set("bytes", static_cast<int>(wav.size()));
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }
}

} // namespace fbzz::editor
