/// @file    SynthAsset.cpp
/// @brief   .synth の TOML 入出力と Inspector 反射。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Engine/Asset/SynthAsset.hpp>
#include <Engine/Audio/Synth.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <sstream>

namespace fbzz::asset {
namespace {

constexpr const char* kWaveLabels[] = { "Sine", "Square", "Saw", "Triangle", "Noise" };

} // namespace

void SynthAsset::Reflect(scene::IReflector& r)
{
    int wave = static_cast<int>(spec.wave);
    r.Enum("wave", wave, kWaveLabels);
    spec.wave = static_cast<audio::SynthWave>(wave);

    r.Group("Envelope");
    r.FloatRange("attack",  spec.attack,  0.0f, 5.0f);
    r.FloatRange("sustain", spec.sustain, 0.0f, 5.0f);
    r.FloatRange("decay",   spec.decay,   0.0f, 5.0f);
    r.FloatRange("punch",   spec.punch,   0.0f, 2.0f);

    r.Group("Pitch");
    r.FloatRange("startFrequency", spec.startFrequency, 20.0f, 18000.0f);
    r.FloatRange("minFrequency",   spec.minFrequency,    0.0f, 18000.0f);
    r.FloatRange("slide",          spec.slide,         -32.0f, 32.0f);
    r.Tooltip("1 秒あたりのオクターブ変化。負で下降。");
    r.FloatRange("vibratoDepth",   spec.vibratoDepth,    0.0f, 1.0f);
    r.FloatRange("vibratoRate",    spec.vibratoRate,     0.0f, 100.0f);

    r.Group("Timbre");
    r.FloatRange("dutyCycle",      spec.dutyCycle,      0.01f, 0.99f);
    r.FloatRange("dutySweep",      spec.dutySweep,      -8.0f, 8.0f);
    r.FloatRange("lowPassCutoff",  spec.lowPassCutoff,   0.0f, 1.0f);
    r.FloatRange("lowPassSweep",   spec.lowPassSweep,   -8.0f, 8.0f);
    r.FloatRange("highPassCutoff", spec.highPassCutoff,  0.0f, 1.0f);
    r.FloatRange("bitCrush",       spec.bitCrush,        0.0f, 1.0f);

    r.Group("Repeat");
    r.FloatRange("repeatRate",  spec.repeatRate,  0.0f, 100.0f);
    r.FloatRange("arpeggioMod", spec.arpeggioMod, 0.0f, 4.0f);

    r.Group("Output");
    r.FloatRange("amplitude", spec.amplitude, 0.0f, 1.0f);
    int seed = static_cast<int>(spec.seed);
    r.Field("seed", seed);
    spec.seed = static_cast<uint32_t>(seed);
    r.Readonly("preset", presetName);
}

bool LoadSynthAssetFromFile(std::string_view path, SynthAsset& outAsset)
{
    const std::string pathString(path);
    std::string text;
    if (!util::FileSystem::ReadText(pathString, text)) {
        FBZZ_LOG_WARN("SynthAsset: cannot open [%s]", pathString.c_str());
        return false;
    }

    toml::parse_result parsed = toml::parse(text, pathString);
    if (!parsed) {
        FBZZ_LOG_WARN("SynthAsset: parse failed [%s]", pathString.c_str());
        return false;
    }

    /// @note パラメーターは今後増えるため、欠損キーは既定のままにする。書いていない項目を
    ///       エラーにすると、既存の .synth が engine 更新のたびに読めなくなる。
    const toml::table& root = parsed.table();
    const toml::table* table = root["synth"].as_table();
    if (!table) table = &root;

    SynthAsset asset;
    audio::SynthSpec& spec = asset.spec;

    const auto readFloat = [table](const char* key, float& value) {
        value = static_cast<float>((*table)[key].value_or(static_cast<double>(value)));
    };

    spec.wave = audio::WaveFromString(
        (*table)["wave"].value_or(std::string{}).c_str(), spec.wave);

    readFloat("attack",         spec.attack);
    readFloat("sustain",        spec.sustain);
    readFloat("decay",          spec.decay);
    readFloat("punch",          spec.punch);
    readFloat("startFrequency", spec.startFrequency);
    readFloat("minFrequency",   spec.minFrequency);
    readFloat("slide",          spec.slide);
    readFloat("vibratoDepth",   spec.vibratoDepth);
    readFloat("vibratoRate",    spec.vibratoRate);
    readFloat("dutyCycle",      spec.dutyCycle);
    readFloat("dutySweep",      spec.dutySweep);
    readFloat("lowPassCutoff",  spec.lowPassCutoff);
    readFloat("lowPassSweep",   spec.lowPassSweep);
    readFloat("highPassCutoff", spec.highPassCutoff);
    readFloat("bitCrush",       spec.bitCrush);
    readFloat("repeatRate",     spec.repeatRate);
    readFloat("arpeggioMod",    spec.arpeggioMod);
    readFloat("amplitude",      spec.amplitude);

    spec.seed       = static_cast<uint32_t>((*table)["seed"].value_or(int64_t{ 0 }));
    spec.sampleRate = static_cast<uint32_t>((*table)["sampleRate"].value_or(int64_t{ 0 }));
    asset.presetName = (*table)["preset"].value_or(std::string{});

    outAsset = std::move(asset);
    return true;
}

bool SaveSynthAssetToFile(std::string_view path, const SynthAsset& asset)
{
    const std::string pathString(path);
    const audio::SynthSpec& spec = asset.spec;

    /// @note 全項目を書くと「何も触っていないのに 20 行」になり、git の差分から
    ///       「どこを調整したか」が読めなくなるため、既定値と同じ項目は書かない (FbxMetaSerializer と同方針)。
    const audio::SynthSpec defaults;
    toml::table table;
    const auto writeFloat = [&table](const char* key, float value, float defaultValue) {
        if (value != defaultValue) table.insert(key, static_cast<double>(value));
    };

    if (spec.wave != defaults.wave)
        table.insert("wave", std::string(audio::WaveToString(spec.wave)));

    writeFloat("attack",         spec.attack,         defaults.attack);
    writeFloat("sustain",        spec.sustain,        defaults.sustain);
    writeFloat("decay",          spec.decay,          defaults.decay);
    writeFloat("punch",          spec.punch,          defaults.punch);
    writeFloat("startFrequency", spec.startFrequency, defaults.startFrequency);
    writeFloat("minFrequency",   spec.minFrequency,   defaults.minFrequency);
    writeFloat("slide",          spec.slide,          defaults.slide);
    writeFloat("vibratoDepth",   spec.vibratoDepth,   defaults.vibratoDepth);
    writeFloat("vibratoRate",    spec.vibratoRate,    defaults.vibratoRate);
    writeFloat("dutyCycle",      spec.dutyCycle,      defaults.dutyCycle);
    writeFloat("dutySweep",      spec.dutySweep,      defaults.dutySweep);
    writeFloat("lowPassCutoff",  spec.lowPassCutoff,  defaults.lowPassCutoff);
    writeFloat("lowPassSweep",   spec.lowPassSweep,   defaults.lowPassSweep);
    writeFloat("highPassCutoff", spec.highPassCutoff, defaults.highPassCutoff);
    writeFloat("bitCrush",       spec.bitCrush,       defaults.bitCrush);
    writeFloat("repeatRate",     spec.repeatRate,     defaults.repeatRate);
    writeFloat("arpeggioMod",    spec.arpeggioMod,    defaults.arpeggioMod);
    writeFloat("amplitude",      spec.amplitude,      defaults.amplitude);

    if (spec.seed != 0)       table.insert("seed",       static_cast<int64_t>(spec.seed));
    if (spec.sampleRate != 0) table.insert("sampleRate", static_cast<int64_t>(spec.sampleRate));
    if (!asset.presetName.empty()) table.insert("preset", asset.presetName);

    toml::table root;
    root.insert("synth", std::move(table));

    if (!util::FileSystem::EnsureParentDirectory(util::FileSystem::PathFromUtf8(pathString))) {
        FBZZ_LOG_ERROR("SynthAsset: cannot create dir for [%s]", pathString.c_str());
        return false;
    }

    std::ostringstream oss;
    oss << root;
    if (!util::FileSystem::WriteText(pathString, oss.str())) {
        FBZZ_LOG_ERROR("SynthAsset: write failed [%s]", pathString.c_str());
        return false;
    }
    return true;
}

} // namespace fbzz::asset
