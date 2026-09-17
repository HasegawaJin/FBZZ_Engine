/// @file    SequenceAsset.cpp
/// @brief   .sequence (TOML) の読み書きと尺の導出
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Asset/SequenceAsset.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <initializer_list>
#include <span>
#include <sstream>
#include <utility>

namespace fbzz::asset {
namespace {

constexpr int64_t kSequenceFormatVersion = 1;

/// @name 列挙 ⇄ 文字列
/// @note .sequence は人が読んで尺を追う対象で diff にも出るため、数値ではなく名前で書く。
///       type = 3 では何のトラックか分からず、列挙へ値を挿し込んだ瞬間に全ファイルがずれる。

struct EnumName {
    const char* name;
    int         value;
};

int NameToEnum(std::string_view name, std::span<const EnumName> table, int fallback)
{
    for (const auto& entry : table)
        if (name == entry.name) return entry.value;
    return fallback;
}

const char* EnumToName(int value, std::span<const EnumName> table, const char* fallback)
{
    for (const auto& entry : table)
        if (entry.value == value) return entry.name;
    return fallback;
}

constexpr EnumName kTrackTypes[] = {
    { "Animation",  static_cast<int>(SequenceTrackType::Animation)  },
    { "Transform",  static_cast<int>(SequenceTrackType::Transform)  },
    { "Property",   static_cast<int>(SequenceTrackType::Property)   },
    { "Activation", static_cast<int>(SequenceTrackType::Activation) },
    { "Audio",      static_cast<int>(SequenceTrackType::Audio)      },
    { "VFX",        static_cast<int>(SequenceTrackType::VFX)        },
    { "Event",      static_cast<int>(SequenceTrackType::Event)      },
};

constexpr EnumName kWrapModes[] = {
    { "Once",    static_cast<int>(SequenceWrapMode::Once)    },
    { "Loop",    static_cast<int>(SequenceWrapMode::Loop)    },
    { "HoldEnd", static_cast<int>(SequenceWrapMode::HoldEnd) },
};

constexpr EnumName kTimeModes[] = {
    { "Scaled",   static_cast<int>(SequenceTimeMode::Scaled)   },
    { "Unscaled", static_cast<int>(SequenceTimeMode::Unscaled) },
};

constexpr EnumName kSpaces[] = {
    { "Local",           static_cast<int>(SequenceTransformSpace::Local)           },
    { "World",           static_cast<int>(SequenceTransformSpace::World)           },
    { "RelativeToStart", static_cast<int>(SequenceTransformSpace::RelativeToStart) },
};

constexpr EnumName kInterps[] = {
    { "Step",   static_cast<int>(AnimInterp::Step)   },
    { "Linear", static_cast<int>(AnimInterp::Linear) },
    { "Cubic",  static_cast<int>(AnimInterp::Cubic)  },
};

constexpr EnumName kTargetTypes[] = {
    { "Component", static_cast<int>(AnimTargetType::ComponentProperty) },
    { "Material",  static_cast<int>(AnimTargetType::MaterialProperty)  },
    { "Morph",     static_cast<int>(AnimTargetType::MorphWeight)       },
};

constexpr EnumName kValueTypes[] = {
    { "Float",   static_cast<int>(AnimValueType::Float)   },
    { "Vector2", static_cast<int>(AnimValueType::Vector2) },
    { "Vector3", static_cast<int>(AnimValueType::Vector3) },
    { "Vector4", static_cast<int>(AnimValueType::Vector4) },
    { "Color",   static_cast<int>(AnimValueType::Color)   },
    { "Int",     static_cast<int>(AnimValueType::Int)     },
    { "Bool",    static_cast<int>(AnimValueType::Bool)    },
};

/// @name 読み書きの小道具

double ReadDouble(const toml::table& table, std::string_view key, double fallback)
{
    return table[key].value_or(fallback);
}

float ReadFloat(const toml::table& table, std::string_view key, double fallback)
{
    return static_cast<float>(table[key].value_or(fallback));
}

toml::array WriteFloats(std::initializer_list<float> values)
{
    toml::array array;
    for (const float value : values) array.push_back(static_cast<double>(value));
    return array;
}

/// キー配列の要素は { time = .., value = [..] } の形で統一する。
template<typename Key, typename ReadValue>
void ReadKeys(const toml::table& table, std::string_view name,
              std::vector<Key>& outKeys, ReadValue readValue)
{
    const toml::array* array = table[name].as_array();
    if (!array) return;
    outKeys.clear();
    outKeys.reserve(array->size());
    for (const auto& element : *array) {
        const toml::table* keyTable = element.as_table();
        if (!keyTable) continue;
        Key key;
        key.time = ReadDouble(*keyTable, "time", 0.0);
        readValue(*keyTable, key);
        outKeys.push_back(std::move(key));
    }
    std::stable_sort(outKeys.begin(), outKeys.end(),
                     [](const Key& a, const Key& b) { return a.time < b.time; });
}

math::Vector3 ReadVector3(const toml::table& table, std::string_view key)
{
    const toml::array* array = table[key].as_array();
    if (!array || array->size() < 3) return math::Vector3::ZERO;
    return { static_cast<float>((*array)[0].value_or(0.0)),
             static_cast<float>((*array)[1].value_or(0.0)),
             static_cast<float>((*array)[2].value_or(0.0)) };
}

math::Vector4 ReadVector4(const toml::table& table, std::string_view key, float defaultW)
{
    const toml::array* array = table[key].as_array();
    if (!array || array->size() < 4)
        return { 0.0f, 0.0f, 0.0f, defaultW };
    return { static_cast<float>((*array)[0].value_or(0.0)),
             static_cast<float>((*array)[1].value_or(0.0)),
             static_cast<float>((*array)[2].value_or(0.0)),
             static_cast<float>((*array)[3].value_or(static_cast<double>(defaultW))) };
}

math::Vector2 ReadVector2(const toml::table& table, std::string_view key)
{
    const toml::array* array = table[key].as_array();
    if (!array || array->size() < 2) return math::Vector2::ZERO;
    return { static_cast<float>((*array)[0].value_or(0.0)),
             static_cast<float>((*array)[1].value_or(0.0)) };
}

/// @name Property トラック

void ReadPropertyTrack(const toml::table& table, PropertyAnimationTrack& out)
{
    out.targetType = static_cast<AnimTargetType>(NameToEnum(
        table["targetType"].value_or(std::string{ "Component" }), kTargetTypes,
        static_cast<int>(AnimTargetType::ComponentProperty)));
    out.valueType = static_cast<AnimValueType>(NameToEnum(
        table["valueType"].value_or(std::string{ "Float" }), kValueTypes,
        static_cast<int>(AnimValueType::Float)));
    out.interp = static_cast<AnimInterp>(NameToEnum(
        table["interp"].value_or(std::string{ "Linear" }), kInterps,
        static_cast<int>(AnimInterp::Linear)));
    out.componentType = table["componentType"].value_or(std::string{});
    out.propertyName  = table["property"].value_or(std::string{});
    out.materialSlot  = static_cast<int32_t>(table["materialSlot"].value_or(int64_t{ 0 }));
    out.meshIndex     = static_cast<int32_t>(table["meshIndex"].value_or(int64_t{ 0 }));

    ReadKeys(table, "floatKeys", out.floatKeys, [](const toml::table& t, FloatKey& key) {
        key.value      = ReadFloat(t, "value", 0.0);
        key.inTangent  = ReadFloat(t, "inTangent", 0.0);
        key.outTangent = ReadFloat(t, "outTangent", 0.0);
    });
    ReadKeys(table, "vector2Keys", out.vector2Keys, [](const toml::table& t, Vector2Key& key) {
        key.value = ReadVector2(t, "value");
    });
    ReadKeys(table, "vector3Keys", out.vector3Keys, [](const toml::table& t, VectorKey& key) {
        key.value = ReadVector3(t, "value");
    });
    ReadKeys(table, "vector4Keys", out.vector4Keys, [](const toml::table& t, Vector4Key& key) {
        key.value = ReadVector4(t, "value", 1.0f);
    });
    ReadKeys(table, "intKeys", out.intKeys, [](const toml::table& t, IntKey& key) {
        key.value = static_cast<int32_t>(t["value"].value_or(int64_t{ 0 }));
    });
    ReadKeys(table, "boolKeys", out.boolKeys, [](const toml::table& t, BoolKey& key) {
        key.value = t["value"].value_or(false);
    });
}

void WritePropertyTrack(const PropertyAnimationTrack& track, toml::table& out)
{
    out.insert("targetType",
               EnumToName(static_cast<int>(track.targetType), kTargetTypes, "Component"));
    out.insert("valueType",
               EnumToName(static_cast<int>(track.valueType), kValueTypes, "Float"));
    out.insert("interp", EnumToName(static_cast<int>(track.interp), kInterps, "Linear"));
    out.insert("componentType", track.componentType);
    out.insert("property", track.propertyName);
    out.insert("materialSlot", static_cast<int64_t>(track.materialSlot));
    out.insert("meshIndex", static_cast<int64_t>(track.meshIndex));

    const auto writeKeys = [&out](const char* name, auto& keys, auto writeValue) {
        if (keys.empty()) return;
        toml::array array;
        for (const auto& key : keys) {
            toml::table keyTable;
            keyTable.insert("time", key.time);
            writeValue(keyTable, key);
            array.push_back(std::move(keyTable));
        }
        out.insert(name, std::move(array));
    };

    writeKeys("floatKeys", track.floatKeys, [](toml::table& t, const FloatKey& key) {
        t.insert("value", static_cast<double>(key.value));
        if (key.inTangent  != 0.0f) t.insert("inTangent",  static_cast<double>(key.inTangent));
        if (key.outTangent != 0.0f) t.insert("outTangent", static_cast<double>(key.outTangent));
    });
    writeKeys("vector2Keys", track.vector2Keys, [](toml::table& t, const Vector2Key& key) {
        t.insert("value", WriteFloats({ key.value.x, key.value.y }));
    });
    writeKeys("vector3Keys", track.vector3Keys, [](toml::table& t, const VectorKey& key) {
        t.insert("value", WriteFloats({ key.value.x, key.value.y, key.value.z }));
    });
    writeKeys("vector4Keys", track.vector4Keys, [](toml::table& t, const Vector4Key& key) {
        t.insert("value", WriteFloats({ key.value.x, key.value.y, key.value.z, key.value.w }));
    });
    writeKeys("intKeys", track.intKeys, [](toml::table& t, const IntKey& key) {
        t.insert("value", static_cast<int64_t>(key.value));
    });
    writeKeys("boolKeys", track.boolKeys, [](toml::table& t, const BoolKey& key) {
        t.insert("value", key.value);
    });
}

/// @name トラック

SequenceTrack ReadTrack(const toml::table& table)
{
    SequenceTrack track;
    track.type = static_cast<SequenceTrackType>(NameToEnum(
        table["type"].value_or(std::string{ "Event" }), kTrackTypes,
        static_cast<int>(SequenceTrackType::Event)));
    track.name          = table["name"].value_or(std::string{});
    track.binding       = table["binding"].value_or(std::string{});
    track.muted         = table["muted"].value_or(false);
    track.restoreOnStop = table["restoreOnStop"].value_or(true);

    switch (track.type) {
    case SequenceTrackType::Animation: {
        track.layerName = table["layer"].value_or(std::string{ "Base" });
        if (const toml::array* clips = table["clips"].as_array()) {
            for (const auto& element : *clips) {
                const toml::table* clipTable = element.as_table();
                if (!clipTable) continue;
                SequenceAnimationClip clip;
                clip.start      = ReadDouble(*clipTable, "start", 0.0);
                clip.duration   = ReadDouble(*clipTable, "duration", 0.0);
                clip.sourcePath = (*clipTable)["source"].value_or(std::string{});
                clip.clipName   = (*clipTable)["clip"].value_or(std::string{});
                clip.speed      = ReadFloat(*clipTable, "speed", 1.0);
                clip.loop       = (*clipTable)["loop"].value_or(false);
                clip.clipIn     = ReadDouble(*clipTable, "clipIn", 0.0);
                clip.blendIn    = ReadFloat(*clipTable, "blendIn", 0.0);
                clip.blendOut   = ReadFloat(*clipTable, "blendOut", 0.0);
                track.animationClips.push_back(std::move(clip));
            }
            std::stable_sort(track.animationClips.begin(), track.animationClips.end(),
                             [](const SequenceAnimationClip& a, const SequenceAnimationClip& b) {
                                 return a.start < b.start;
                             });
        }
        break;
    }
    case SequenceTrackType::Transform: {
        track.space = static_cast<SequenceTransformSpace>(NameToEnum(
            table["space"].value_or(std::string{ "Local" }), kSpaces,
            static_cast<int>(SequenceTransformSpace::Local)));
        track.interp = static_cast<AnimInterp>(NameToEnum(
            table["interp"].value_or(std::string{ "Cubic" }), kInterps,
            static_cast<int>(AnimInterp::Cubic)));
        ReadKeys(table, "positions", track.positions, [](const toml::table& t, VectorKey& key) {
            key.value = ReadVector3(t, "value");
        });
        ReadKeys(table, "rotations", track.rotations,
                 [](const toml::table& t, QuaternionKey& key) {
                     const math::Vector4 v = ReadVector4(t, "value", 1.0f);
                     key.value = math::Quaternion{ v.x, v.y, v.z, v.w };
                 });
        ReadKeys(table, "scales", track.scales, [](const toml::table& t, VectorKey& key) {
            key.value = ReadVector3(t, "value");
        });
        break;
    }
    case SequenceTrackType::Property:
        ReadPropertyTrack(table, track.property);
        break;
    case SequenceTrackType::Activation: {
        if (const toml::array* ranges = table["ranges"].as_array()) {
            for (const auto& element : *ranges) {
                const toml::table* rangeTable = element.as_table();
                if (!rangeTable) continue;
                track.ranges.push_back(SequenceRange{
                    ReadDouble(*rangeTable, "start", 0.0),
                    ReadDouble(*rangeTable, "end", 0.0) });
            }
        }
        break;
    }
    case SequenceTrackType::Audio: {
        if (const toml::array* clips = table["clips"].as_array()) {
            for (const auto& element : *clips) {
                const toml::table* clipTable = element.as_table();
                if (!clipTable) continue;
                SequenceAudioClip clip;
                clip.start    = ReadDouble(*clipTable, "start", 0.0);
                clip.clipPath = (*clipTable)["clip"].value_or(std::string{});
                clip.bus      = (*clipTable)["bus"].value_or(std::string{ "SE" });
                clip.volume   = ReadFloat(*clipTable, "volume", 1.0);
                clip.loop     = (*clipTable)["loop"].value_or(false);
                clip.end      = ReadDouble(*clipTable, "end", 0.0);
                track.audioClips.push_back(std::move(clip));
            }
        }
        break;
    }
    case SequenceTrackType::VFX: {
        if (const toml::array* clips = table["clips"].as_array()) {
            for (const auto& element : *clips) {
                const toml::table* clipTable = element.as_table();
                if (!clipTable) continue;
                SequenceVfxClip clip;
                clip.start   = ReadDouble(*clipTable, "start", 0.0);
                clip.end     = ReadDouble(*clipTable, "end", 0.0);
                clip.restart = (*clipTable)["restart"].value_or(true);
                track.vfxClips.push_back(std::move(clip));
            }
        }
        break;
    }
    case SequenceTrackType::Event: {
        if (const toml::array* keys = table["keys"].as_array()) {
            for (const auto& element : *keys) {
                const toml::table* keyTable = element.as_table();
                if (!keyTable) continue;
                SequenceEventKey key;
                key.time       = ReadDouble(*keyTable, "time", 0.0);
                key.name       = (*keyTable)["name"].value_or(std::string{});
                key.intParam   = static_cast<int32_t>((*keyTable)["int"].value_or(int64_t{ 0 }));
                key.floatParam = ReadFloat(*keyTable, "float", 0.0);
                track.eventKeys.push_back(std::move(key));
            }
            std::stable_sort(track.eventKeys.begin(), track.eventKeys.end(),
                             [](const SequenceEventKey& a, const SequenceEventKey& b) {
                                 return a.time < b.time;
                             });
        }
        break;
    }
    }
    return track;
}

toml::table WriteTrack(const SequenceTrack& track)
{
    toml::table table;
    table.insert("type", EnumToName(static_cast<int>(track.type), kTrackTypes, "Event"));
    table.insert("name", track.name);
    table.insert("binding", track.binding);
    if (track.muted) table.insert("muted", true);
    if (!track.restoreOnStop) table.insert("restoreOnStop", false);

    switch (track.type) {
    case SequenceTrackType::Animation: {
        table.insert("layer", track.layerName);
        toml::array clips;
        for (const auto& clip : track.animationClips) {
            toml::table clipTable;
            clipTable.insert("start", clip.start);
            if (clip.duration > 0.0) clipTable.insert("duration", clip.duration);
            clipTable.insert("source", clip.sourcePath);
            clipTable.insert("clip", clip.clipName);
            if (clip.speed != 1.0f) clipTable.insert("speed", static_cast<double>(clip.speed));
            if (clip.loop) clipTable.insert("loop", true);
            if (clip.clipIn != 0.0) clipTable.insert("clipIn", clip.clipIn);
            if (clip.blendIn  != 0.0f) clipTable.insert("blendIn",  static_cast<double>(clip.blendIn));
            if (clip.blendOut != 0.0f) clipTable.insert("blendOut", static_cast<double>(clip.blendOut));
            clips.push_back(std::move(clipTable));
        }
        table.insert("clips", std::move(clips));
        break;
    }
    case SequenceTrackType::Transform: {
        table.insert("space", EnumToName(static_cast<int>(track.space), kSpaces, "Local"));
        table.insert("interp", EnumToName(static_cast<int>(track.interp), kInterps, "Cubic"));
        const auto writeKeys = [&table](const char* name, const auto& keys, auto writeValue) {
            if (keys.empty()) return;
            toml::array array;
            for (const auto& key : keys) {
                toml::table keyTable;
                keyTable.insert("time", key.time);
                writeValue(keyTable, key);
                array.push_back(std::move(keyTable));
            }
            table.insert(name, std::move(array));
        };
        writeKeys("positions", track.positions, [](toml::table& t, const VectorKey& key) {
            t.insert("value", WriteFloats({ key.value.x, key.value.y, key.value.z }));
        });
        writeKeys("rotations", track.rotations, [](toml::table& t, const QuaternionKey& key) {
            t.insert("value", WriteFloats({ key.value.x, key.value.y, key.value.z, key.value.w }));
        });
        writeKeys("scales", track.scales, [](toml::table& t, const VectorKey& key) {
            t.insert("value", WriteFloats({ key.value.x, key.value.y, key.value.z }));
        });
        break;
    }
    case SequenceTrackType::Property:
        WritePropertyTrack(track.property, table);
        break;
    case SequenceTrackType::Activation: {
        toml::array ranges;
        for (const auto& range : track.ranges) {
            toml::table rangeTable;
            rangeTable.insert("start", range.start);
            rangeTable.insert("end", range.end);
            ranges.push_back(std::move(rangeTable));
        }
        table.insert("ranges", std::move(ranges));
        break;
    }
    case SequenceTrackType::Audio: {
        toml::array clips;
        for (const auto& clip : track.audioClips) {
            toml::table clipTable;
            clipTable.insert("start", clip.start);
            clipTable.insert("clip", clip.clipPath);
            clipTable.insert("bus", clip.bus);
            clipTable.insert("volume", static_cast<double>(clip.volume));
            if (clip.loop) {
                clipTable.insert("loop", true);
                clipTable.insert("end", clip.end);
            }
            clips.push_back(std::move(clipTable));
        }
        table.insert("clips", std::move(clips));
        break;
    }
    case SequenceTrackType::VFX: {
        toml::array clips;
        for (const auto& clip : track.vfxClips) {
            toml::table clipTable;
            clipTable.insert("start", clip.start);
            clipTable.insert("end", clip.end);
            clipTable.insert("restart", clip.restart);
            clips.push_back(std::move(clipTable));
        }
        table.insert("clips", std::move(clips));
        break;
    }
    case SequenceTrackType::Event: {
        toml::array keys;
        for (const auto& key : track.eventKeys) {
            toml::table keyTable;
            keyTable.insert("time", key.time);
            keyTable.insert("name", key.name);
            if (key.intParam   != 0)    keyTable.insert("int", static_cast<int64_t>(key.intParam));
            if (key.floatParam != 0.0f) keyTable.insert("float", static_cast<double>(key.floatParam));
            keys.push_back(std::move(keyTable));
        }
        table.insert("keys", std::move(keys));
        break;
    }
    }
    return table;
}

template<typename Container, typename TimeOf>
double MaxTime(const Container& items, TimeOf timeOf, double current)
{
    for (const auto& item : items) current = (std::max)(current, timeOf(item));
    return current;
}

} // namespace

double SequenceAsset::GetDurationSeconds() const
{
    if (duration > 0.0) return duration;

    /// @note 尺を書き忘れた .sequence が「再生した瞬間に終わる」形で壊れると、原因が
    ///       トラック側に見えて追いにくいため、最も遅いキーまでは必ず回して導出する。
    double derived = 0.0;
    for (const auto& track : tracks) {
        derived = MaxTime(track.animationClips,
                          [](const SequenceAnimationClip& c) { return c.start + c.duration; },
                          derived);
        derived = MaxTime(track.positions, [](const VectorKey& k) { return k.time; }, derived);
        derived = MaxTime(track.rotations, [](const QuaternionKey& k) { return k.time; }, derived);
        derived = MaxTime(track.scales,    [](const VectorKey& k) { return k.time; }, derived);
        derived = MaxTime(track.ranges,    [](const SequenceRange& r) { return r.end; }, derived);
        derived = MaxTime(track.audioClips,
                          [](const SequenceAudioClip& c) { return (std::max)(c.start, c.end); },
                          derived);
        derived = MaxTime(track.vfxClips,
                          [](const SequenceVfxClip& c) { return (std::max)(c.start, c.end); },
                          derived);
        derived = MaxTime(track.eventKeys, [](const SequenceEventKey& k) { return k.time; }, derived);
        derived = MaxTime(track.property.floatKeys,   [](const FloatKey& k) { return k.time; }, derived);
        derived = MaxTime(track.property.vector2Keys, [](const Vector2Key& k) { return k.time; }, derived);
        derived = MaxTime(track.property.vector3Keys, [](const VectorKey& k) { return k.time; }, derived);
        derived = MaxTime(track.property.vector4Keys, [](const Vector4Key& k) { return k.time; }, derived);
        derived = MaxTime(track.property.intKeys,     [](const IntKey& k) { return k.time; }, derived);
        derived = MaxTime(track.property.boolKeys,    [](const BoolKey& k) { return k.time; }, derived);
    }
    return derived;
}

const char* SequenceTrackTypeName(SequenceTrackType type)
{
    return EnumToName(static_cast<int>(type), kTrackTypes, "Event");
}

SequenceTrackType SequenceTrackTypeFromName(std::string_view name)
{
    return static_cast<SequenceTrackType>(
        NameToEnum(name, kTrackTypes, static_cast<int>(SequenceTrackType::Event)));
}

bool LoadSequenceAsset(const std::string& path, SequenceAsset& outAsset)
{
    std::string text;
    if (!util::FileSystem::ReadText(AssetManager::ResolveAssetPath(path), text)) return false;
    toml::parse_result result = toml::parse(text);
    if (!result) return false;

    DecodeGuidRefs(result.table());

    SequenceAsset loaded;
    if (const toml::table* header = result["sequence"].as_table()) {
        loaded.name     = (*header)["name"].value_or(std::string{});
        loaded.duration = ReadDouble(*header, "duration", 0.0);
        loaded.wrapMode = static_cast<SequenceWrapMode>(NameToEnum(
            (*header)["wrapMode"].value_or(std::string{ "Once" }), kWrapModes,
            static_cast<int>(SequenceWrapMode::Once)));
        loaded.timeMode = static_cast<SequenceTimeMode>(NameToEnum(
            (*header)["timeMode"].value_or(std::string{ "Scaled" }), kTimeModes,
            static_cast<int>(SequenceTimeMode::Scaled)));
    }

    if (const toml::array* tracks = result["tracks"].as_array())
        for (const auto& element : *tracks)
            if (const toml::table* trackTable = element.as_table())
                loaded.tracks.push_back(ReadTrack(*trackTable));

    outAsset = std::move(loaded);
    return true;
}

bool SaveSequenceAsset(const std::string& path, const SequenceAsset& asset)
{
    toml::table root;
    root.insert("version", kSequenceFormatVersion);

    toml::table header;
    header.insert("name", asset.name);
    header.insert("duration", asset.duration);
    header.insert("wrapMode",
                  EnumToName(static_cast<int>(asset.wrapMode), kWrapModes, "Once"));
    header.insert("timeMode",
                  EnumToName(static_cast<int>(asset.timeMode), kTimeModes, "Scaled"));
    root.insert("sequence", std::move(header));

    toml::array tracks;
    for (const auto& track : asset.tracks)
        tracks.push_back(WriteTrack(track));
    root.insert("tracks", std::move(tracks));

    /// @note クリップ・音源の参照を guid: へ寄せる (リネーム・移動耐性)。
    EncodeGuidRefs(root);

    std::ostringstream stream;
    stream << root;
    return util::FileSystem::WriteText(AssetManager::ResolveAssetPath(path), stream.str());
}

} // namespace fbzz::asset
