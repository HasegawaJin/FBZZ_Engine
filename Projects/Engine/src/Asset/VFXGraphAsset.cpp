// FBZZ Engine
// VFXGraphAsset.cpp | fbzz::asset
// .vfxグラフのTOML入出力、参照GUID変換、DAG検証と時間スケジュール構築
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/VFXAuthoringSchema.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::asset {
namespace {

void SetError(std::string* outError, const std::string& message)
{
    if (outError != nullptr) *outError = message;
}

bool IsBindingTypeCompatible(VFXParamType parameterType, reflection::PropertyType propertyType)
{
    switch (parameterType) {
    case VFXParamType::Float:
        return propertyType == reflection::PropertyType::Float
            || propertyType == reflection::PropertyType::Curve;
    case VFXParamType::Int: return propertyType == reflection::PropertyType::Int;
    case VFXParamType::Bool: return propertyType == reflection::PropertyType::Bool;
    case VFXParamType::Color:
        return propertyType == reflection::PropertyType::Color
            || propertyType == reflection::PropertyType::Vector3
            || propertyType == reflection::PropertyType::Gradient;
    case VFXParamType::Vector3: return propertyType == reflection::PropertyType::Vector3;
    case VFXParamType::AssetRef:
        return propertyType == reflection::PropertyType::AssetRef
            || propertyType == reflection::PropertyType::String;
    }
    return false;
}

bool IsParamValueCompatible(VFXParamType type, const VFXParamValue& value)
{
    if (const auto* constant = std::get_if<VFXConstant>(&value.source)) {
        switch (type) {
        case VFXParamType::Float: return std::holds_alternative<float>(*constant);
        case VFXParamType::Int: return std::holds_alternative<int>(*constant);
        case VFXParamType::Bool: return std::holds_alternative<bool>(*constant);
        case VFXParamType::Color: return std::holds_alternative<math::Vector4>(*constant);
        case VFXParamType::Vector3: return std::holds_alternative<math::Vector3>(*constant);
        case VFXParamType::AssetRef: return std::holds_alternative<std::string>(*constant);
        }
    }
    if (std::holds_alternative<VFXCurveSource>(value.source)) return type == VFXParamType::Float;
    if (std::holds_alternative<VFXGradientSource>(value.source)) return type == VFXParamType::Color;
    if (std::holds_alternative<VFXRandomRange>(value.source))
        return type == VFXParamType::Float || type == VFXParamType::Int;
    return std::holds_alternative<VFXAttributeRef>(value.source)
        || std::holds_alternative<VFXSignalRef>(value.source);
}

toml::array WriteVector3(const math::Vector3& value)
{
    return toml::array{ value.x, value.y, value.z };
}

toml::array WriteVector4(const math::Vector4& value)
{
    return toml::array{ value.x, value.y, value.z, value.w };
}

math::Vector3 ReadVector3(const toml::node_view<const toml::node>& value,
                          const math::Vector3& fallback)
{
    const auto* array = value.as_array();
    if (array == nullptr || array->size() < 3) return fallback;
    return {
        static_cast<float>((*array)[0].value_or(static_cast<double>(fallback.x))),
        static_cast<float>((*array)[1].value_or(static_cast<double>(fallback.y))),
        static_cast<float>((*array)[2].value_or(static_cast<double>(fallback.z)))
    };
}

math::Vector4 ReadVector4(const toml::node_view<const toml::node>& value,
                          const math::Vector4& fallback)
{
    const auto* array = value.as_array();
    if (array == nullptr || array->size() < 4) return fallback;
    return {
        static_cast<float>((*array)[0].value_or(static_cast<double>(fallback.x))),
        static_cast<float>((*array)[1].value_or(static_cast<double>(fallback.y))),
        static_cast<float>((*array)[2].value_or(static_cast<double>(fallback.z))),
        static_cast<float>((*array)[3].value_or(static_cast<double>(fallback.w)))
    };
}

toml::table WriteParamValue(const VFXParamValue& value)
{
    toml::table table;
    table.insert("source", static_cast<std::int64_t>(value.source.index()));
    if (const auto* constant = std::get_if<VFXConstant>(&value.source)) {
        table.insert("constantType", static_cast<std::int64_t>(constant->index()));
        if (const auto* item = std::get_if<float>(constant)) table.insert("float", *item);
        else if (const auto* item = std::get_if<int>(constant)) table.insert("int", static_cast<std::int64_t>(*item));
        else if (const auto* item = std::get_if<bool>(constant)) table.insert("bool", *item);
        else if (const auto* item = std::get_if<math::Vector4>(constant)) table.insert("color", WriteVector4(*item));
        else if (const auto* item = std::get_if<math::Vector3>(constant)) table.insert("vector3", WriteVector3(*item));
        else if (const auto* item = std::get_if<std::string>(constant)) table.insert("asset", *item);
    } else if (const auto* curve = std::get_if<VFXCurveSource>(&value.source)) {
        toml::array keys;
        const std::uint32_t count = (std::min)(curve->curve.keyCount, 4u);
        for (std::uint32_t index = 0; index < count; ++index)
            keys.push_back(toml::array{ curve->curve.keys[index].time, curve->curve.keys[index].value });
        table.insert("keys", std::move(keys));
    } else if (const auto* gradient = std::get_if<VFXGradientSource>(&value.source)) {
        toml::array keys;
        const std::uint32_t count = (std::min)(gradient->gradient.keyCount, 4u);
        for (std::uint32_t index = 0; index < count; ++index) {
            toml::table key;
            key.insert("time", gradient->gradient.keys[index].time);
            key.insert("color", WriteVector4(gradient->gradient.keys[index].color));
            keys.push_back(std::move(key));
        }
        table.insert("keys", std::move(keys));
    } else if (const auto* random = std::get_if<VFXRandomRange>(&value.source)) {
        table.insert("minimum", random->minimum);
        table.insert("maximum", random->maximum);
    } else if (const auto* attribute = std::get_if<VFXAttributeRef>(&value.source)) {
        table.insert("path", attribute->path);
    } else if (const auto* signal = std::get_if<VFXSignalRef>(&value.source)) {
        table.insert("signal", signal->signalName);
    }
    return table;
}

VFXParamValue ReadParamValue(const toml::table& table)
{
    VFXParamValue value;
    const int source = static_cast<int>(table["source"].value_or(std::int64_t{0}));
    if (source == 0) {
        const int type = static_cast<int>(table["constantType"].value_or(std::int64_t{0}));
        if (type == 1) value.source = VFXConstant{ static_cast<int>(table["int"].value_or(std::int64_t{0})) };
        else if (type == 2) value.source = VFXConstant{ table["bool"].value_or(false) };
        else if (type == 3) value.source = VFXConstant{ ReadVector4(table["color"], math::Vector4{}) };
        else if (type == 4) value.source = VFXConstant{ ReadVector3(table["vector3"], math::Vector3{}) };
        else if (type == 5) value.source = VFXConstant{ table["asset"].value_or(std::string{}) };
        else value.source = VFXConstant{ static_cast<float>(table["float"].value_or(0.0)) };
    } else if (source == 1) {
        VFXCurveSource curve;
        if (const auto* keys = table["keys"].as_array()) {
            curve.curve.keyCount = static_cast<std::uint32_t>((std::min)(keys->size(), std::size_t{4}));
            for (std::uint32_t index = 0; index < curve.curve.keyCount; ++index) {
                if (const auto* key = (*keys)[index].as_array(); key != nullptr && key->size() >= 2) {
                    curve.curve.keys[index].time = static_cast<float>((*key)[0].value_or(0.0));
                    curve.curve.keys[index].value = static_cast<float>((*key)[1].value_or(0.0));
                }
            }
        }
        value.source = curve;
    } else if (source == 2) {
        VFXGradientSource gradient;
        if (const auto* keys = table["keys"].as_array()) {
            gradient.gradient.keyCount = static_cast<std::uint32_t>((std::min)(keys->size(), std::size_t{4}));
            for (std::uint32_t index = 0; index < gradient.gradient.keyCount; ++index) {
                if (const auto* key = (*keys)[index].as_table()) {
                    gradient.gradient.keys[index].time = static_cast<float>((*key)["time"].value_or(0.0));
                    gradient.gradient.keys[index].color = ReadVector4((*key)["color"], math::Vector4{});
                }
            }
        }
        value.source = gradient;
    } else if (source == 3) {
        value.source = VFXRandomRange{ static_cast<float>(table["minimum"].value_or(0.0)),
                                       static_cast<float>(table["maximum"].value_or(1.0)) };
    } else if (source == 4) value.source = VFXAttributeRef{ table["path"].value_or(std::string{}) };
    else value.source = VFXSignalRef{ table["signal"].value_or(std::string{}) };
    return value;
}

toml::table WriteNode(const VFXGraphNode& node)
{
    toml::table table;
    table.insert("id", static_cast<std::int64_t>(node.id));
    table.insert("type", static_cast<std::int64_t>(node.type));
    table.insert("name", node.name);
    table.insert("editorX", node.editorX);
    table.insert("editorY", node.editorY);
    table.insert("startOffset", node.startOffset);
    table.insert("duration", node.duration);
    table.insert("position", WriteVector3(node.localPosition));
    table.insert("rotation", WriteVector3(node.localRotationDegrees));
    table.insert("scale", WriteVector3(node.localScale));
    table.insert("attachBone", node.attachBone);

    table.insert("particle", SerializeParticleEmitterSettings(node.particle));

    toml::table trail;
    trail.insert("meshPath", node.trail.meshPath);
    trail.insert("materialPath", node.trail.materialPath);
    trail.insert("texturePath", node.trail.texturePath);
    trail.insert("colorStart", WriteVector4(node.trail.colorStart));
    trail.insert("colorEnd", WriteVector4(node.trail.colorEnd));
    trail.insert("lifetime", node.trail.lifetime);
    trail.insert("widthStart", node.trail.widthStart);
    trail.insert("widthEnd", node.trail.widthEnd);
    trail.insert("beamMode", node.trail.beamMode);
    trail.insert("beamStart", WriteVector3(node.trail.beamStart));
    trail.insert("beamEnd", WriteVector3(node.trail.beamEnd));
    table.insert("trail", std::move(trail));

    toml::table light;
    light.insert("color", WriteVector3(node.light.color));
    light.insert("intensity", node.light.intensity);
    light.insert("range", node.light.range);
    table.insert("light", std::move(light));

    toml::table audio;
    audio.insert("clipPath", node.audio.clipPath);
    audio.insert("volume", node.audio.volume);
    audio.insert("pitch", node.audio.pitch);
    audio.insert("spatialBlend", node.audio.spatialBlend);
    audio.insert("loop", node.audio.loop);
    table.insert("audio", std::move(audio));

    toml::table decal;
    decal.insert("albedoPath", node.decal.albedoPath);
    decal.insert("normalPath", node.decal.normalPath);
    decal.insert("emissivePath", node.decal.emissivePath);
    decal.insert("color", WriteVector4(node.decal.color));
    decal.insert("normalStrength", node.decal.normalStrength);
    decal.insert("emissiveScale", node.decal.emissiveScale);
    decal.insert("fadeTime", node.decal.fadeTime);
    table.insert("decal", std::move(decal));
    toml::table subGraph;
    subGraph.insert("graphPath", node.subGraph.graphPath);
    table.insert("subGraph", std::move(subGraph));
    return table;
}

VFXGraphNode ReadNode(const toml::table& table)
{
    VFXGraphNode node;
    node.id = static_cast<int>(table["id"].value_or(std::int64_t{0}));
    node.type = static_cast<VFXNodeType>(table["type"].value_or(std::int64_t{2}));
    node.name = table["name"].value_or(std::string{ "Effect" });
    node.editorX = static_cast<float>(table["editorX"].value_or(0.0));
    node.editorY = static_cast<float>(table["editorY"].value_or(0.0));
    node.startOffset = static_cast<float>(table["startOffset"].value_or(0.0));
    node.duration = static_cast<float>(table["duration"].value_or(1.0));
    node.localPosition = ReadVector3(table["position"], math::Vector3::ZERO);
    node.localRotationDegrees = ReadVector3(table["rotation"], math::Vector3::ZERO);
    node.localScale = ReadVector3(table["scale"], math::Vector3::ONE);
    node.attachBone = table["attachBone"].value_or(std::string{});

    if (const auto* value = table["particle"].as_table()) {
        DeserializeParticleEmitterSettings(*value, node.particle);
        if (!value->contains("duration")) node.particle.duration = node.duration;
    }
    if (const auto* value = table["trail"].as_table()) {
        node.trail.meshPath = (*value)["meshPath"].value_or(std::string{});
        node.trail.materialPath = (*value)["materialPath"].value_or(std::string{});
        node.trail.texturePath = (*value)["texturePath"].value_or(std::string{});
        node.trail.colorStart = ReadVector4((*value)["colorStart"], node.trail.colorStart);
        node.trail.colorEnd = ReadVector4((*value)["colorEnd"], node.trail.colorEnd);
        node.trail.lifetime = static_cast<float>((*value)["lifetime"].value_or(1.0));
        node.trail.widthStart = static_cast<float>((*value)["widthStart"].value_or(0.2));
        node.trail.widthEnd = static_cast<float>((*value)["widthEnd"].value_or(0.02));
        node.trail.beamMode = (*value)["beamMode"].value_or(false);
        node.trail.beamStart = ReadVector3((*value)["beamStart"], node.trail.beamStart);
        node.trail.beamEnd = ReadVector3((*value)["beamEnd"], node.trail.beamEnd);
    }
    if (const auto* value = table["light"].as_table()) {
        node.light.color = ReadVector3((*value)["color"], node.light.color);
        node.light.intensity = static_cast<float>((*value)["intensity"].value_or(4.0));
        node.light.range = static_cast<float>((*value)["range"].value_or(8.0));
    }
    if (const auto* value = table["audio"].as_table()) {
        node.audio.clipPath = (*value)["clipPath"].value_or(std::string{});
        node.audio.volume = static_cast<float>((*value)["volume"].value_or(1.0));
        node.audio.pitch = static_cast<float>((*value)["pitch"].value_or(1.0));
        node.audio.spatialBlend = static_cast<float>((*value)["spatialBlend"].value_or(1.0));
        node.audio.loop = (*value)["loop"].value_or(false);
    }
    if (const auto* value = table["decal"].as_table()) {
        node.decal.albedoPath = (*value)["albedoPath"].value_or(std::string{});
        node.decal.normalPath = (*value)["normalPath"].value_or(std::string{});
        node.decal.emissivePath = (*value)["emissivePath"].value_or(std::string{});
        node.decal.color = ReadVector4((*value)["color"], node.decal.color);
        node.decal.normalStrength = static_cast<float>((*value)["normalStrength"].value_or(1.0));
        node.decal.emissiveScale = static_cast<float>((*value)["emissiveScale"].value_or(0.0));
        node.decal.fadeTime = static_cast<float>((*value)["fadeTime"].value_or(0.25));
    }
    if (const auto* value = table["subGraph"].as_table())
        node.subGraph.graphPath = (*value)["graphPath"].value_or(std::string{});
    return node;
}

} // namespace

const char* VFXNodeTypeName(VFXNodeType type)
{
    switch (type) {
    case VFXNodeType::Entry: return "Entry";
    case VFXNodeType::Delay: return "Delay";
    case VFXNodeType::Particle: return "Particle";
    case VFXNodeType::Trail: return "Trail";
    case VFXNodeType::MeshTrail: return "Mesh Trail";
    case VFXNodeType::Light: return "Light";
    case VFXNodeType::Audio: return "Audio";
    case VFXNodeType::Decal: return "Decal";
    case VFXNodeType::SubGraph: return "Sub Graph";
    }
    return "Unknown";
}

const char* VFXLinkTriggerName(VFXLinkTrigger trigger)
{
    switch (trigger) {
    case VFXLinkTrigger::OnComplete: return "On Complete";
    case VFXLinkTrigger::OnStart: return "On Start";
    case VFXLinkTrigger::OnCollision: return "On Collision";
    case VFXLinkTrigger::OnDeath: return "On Death";
    }
    return "Unknown";
}

VFXGraphBudgetStats CalculateVFXGraphBudget(const VFXGraphAsset& asset)
{
    VFXGraphBudgetStats stats;
    for (const auto& node : asset.nodes) {
        if (node.type == VFXNodeType::Particle)
            stats.particles += (std::max)(node.particle.maxParticles, 0);
        else if (node.type == VFXNodeType::Light)
            ++stats.lights;
        else if (node.type == VFXNodeType::Audio)
            ++stats.audioVoices;
    }
    return stats;
}

std::string SerializeVFXOverrides(const std::vector<VFXParamOverride>& overrides)
{
    toml::table root;
    toml::array values;
    for (const auto& overrideValue : overrides) {
        toml::table table;
        table.insert("paramName", overrideValue.paramName);
        table.insert("value", WriteParamValue(overrideValue.value));
        values.push_back(std::move(table));
    }
    root.insert("values", std::move(values));
    EncodeGuidRefs(root);
    std::ostringstream stream;
    stream << root;
    return stream.str();
}

bool DeserializeVFXOverrides(const std::string& text,
                             std::vector<VFXParamOverride>& overrides)
{
    if (text.empty()) { overrides.clear(); return true; }
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) return false;
    DecodeGuidRefs(parsed.table());
    const auto* values = parsed["values"].as_array();
    if (values == nullptr) return false;
    std::vector<VFXParamOverride> loaded;
    for (const auto& element : *values) {
        const auto* table = element.as_table();
        if (table == nullptr) continue;
        VFXParamOverride overrideValue;
        overrideValue.paramName = (*table)["paramName"].value_or(std::string{});
        if (const auto* value = (*table)["value"].as_table())
            overrideValue.value = ReadParamValue(*value);
        loaded.push_back(std::move(overrideValue));
    }
    overrides = std::move(loaded);
    return true;
}

bool ValidateVFXGraphAsset(const VFXGraphAsset& asset, std::string* outError)
{
    if (asset.nodes.empty()) {
        SetError(outError, "VFX Graphにノードがありません");
        return false;
    }
    if (asset.maxParticles < 1 || asset.maxLights < 0 || asset.maxAudioVoices < 0) {
        SetError(outError, "VFX Graphのbudget値が不正です");
        return false;
    }
    std::unordered_set<int> ids;
    std::unordered_map<int, VFXNodeType> nodeTypes;
    int entryCount = 0;
    for (const auto& node : asset.nodes) {
        if (node.id <= 0 || !ids.insert(node.id).second) {
            SetError(outError, "VFXノードIDが無効または重複しています");
            return false;
        }
        if (node.type == VFXNodeType::Entry) ++entryCount;
        nodeTypes[node.id] = node.type;
        if (node.startOffset < 0.0f || node.duration < 0.0f) {
            SetError(outError, "VFXノードの時間は0以上で指定してください");
            return false;
        }
        if (node.type == VFXNodeType::Particle && node.particle.maxParticles < 1) {
            SetError(outError, "ParticleノードのMax Particlesは1以上で指定してください");
            return false;
        }
        if (node.type == VFXNodeType::SubGraph && node.subGraph.graphPath.empty()) {
            SetError(outError, "Sub Graphノードに.vfx参照が必要です");
            return false;
        }
    }
    if (entryCount != 1) {
        SetError(outError, "VFX GraphにはEntryノードが1個必要です");
        return false;
    }
    const VFXGraphBudgetStats budget = CalculateVFXGraphBudget(asset);
    if (budget.particles > asset.maxParticles || budget.lights > asset.maxLights
        || budget.audioVoices > asset.maxAudioVoices) {
        SetError(outError, "VFX Graphが設定budgetを超過しています");
        return false;
    }
    std::unordered_set<std::string> parameterNames;
    for (const auto& parameter : asset.parameters) {
        if (parameter.name.empty() || !parameterNames.insert(parameter.name).second) {
            SetError(outError, "VFX公開パラメーター名が空または重複しています");
            return false;
        }
        if (parameter.hasRange && parameter.minimum > parameter.maximum) {
            SetError(outError, "VFX公開パラメーターのrangeが不正です");
            return false;
        }
        if (!IsParamValueCompatible(parameter.type, parameter.defaultValue)) {
            SetError(outError, "VFX公開パラメーターのdefault値型が一致しません: " + parameter.name);
            return false;
        }
    }
    std::unordered_set<std::string> bindingKeys;
    for (const auto& binding : asset.bindings) {
        if (!parameterNames.contains(binding.paramName)) {
            SetError(outError, "VFX bindingが存在しない公開パラメーターを参照しています");
            return false;
        }
        const auto node = std::find_if(asset.nodes.begin(), asset.nodes.end(),
            [&](const VFXGraphNode& value) { return value.id == binding.nodeId; });
        if (node == asset.nodes.end()) {
            SetError(outError, "VFX bindingが存在しないノードを参照しています");
            return false;
        }
        reflection::ResolvedProperty resolved;
        if (!reflection::ResolveProperty(GetVFXNodeSchema(), &*node, binding.schemaPath, resolved)
            || resolved.property == nullptr || !resolved.property->exposable) {
            SetError(outError, "VFX bindingのschemaPathが未解決または公開不可です: " + binding.schemaPath);
            return false;
        }
        const auto parameter = std::find_if(asset.parameters.begin(), asset.parameters.end(),
            [&](const VFXParamDefinition& value) { return value.name == binding.paramName; });
        if (!IsBindingTypeCompatible(parameter->type, resolved.property->type)) {
            SetError(outError, "VFX bindingの型が一致しません: " + binding.schemaPath);
            return false;
        }
        const std::string key = binding.paramName + "#" + std::to_string(binding.nodeId)
            + "#" + binding.schemaPath;
        if (!bindingKeys.insert(key).second) {
            SetError(outError, "VFX bindingが重複しています");
            return false;
        }
    }
    std::unordered_set<std::string> variantNames;
    for (const auto& variant : asset.variants) {
        if (variant.name.empty() || !variantNames.insert(variant.name).second) {
            SetError(outError, "VFX Variant名が空または重複しています"); return false;
        }
        std::unordered_set<std::string> overrideNames;
        for (const auto& overrideValue : variant.overrides) {
            const auto parameter = std::find_if(asset.parameters.begin(), asset.parameters.end(),
                [&](const auto& item) { return item.name == overrideValue.paramName; });
            if (parameter == asset.parameters.end() || !overrideNames.insert(overrideValue.paramName).second
                || !IsParamValueCompatible(parameter->type, overrideValue.value)) {
                SetError(outError, "VFX Variant overrideが不正です: " + overrideValue.paramName); return false;
            }
        }
    }
    for (const auto& forward : asset.subGraphForwards) {
        const auto node = std::find_if(asset.nodes.begin(), asset.nodes.end(),
            [&](const auto& item) { return item.id == forward.nodeId; });
        if (node == asset.nodes.end() || node->type != VFXNodeType::SubGraph
            || !parameterNames.contains(forward.parentParam) || forward.childParam.empty()) {
            SetError(outError, "Sub Graph parameter forwardが不正です"); return false;
        }
    }
    std::unordered_set<int> signalIds;
    for (const auto& signal : asset.signalNodes)
        if (signal.id <= 0 || !signalIds.insert(signal.id).second) {
            SetError(outError, "Signal node IDが無効または重複しています"); return false;
        }
    for (const auto& signal : asset.signalNodes) {
        if (static_cast<int>(signal.operation) < 0
            || static_cast<int>(signal.operation) > static_cast<int>(VFXSignalOperation::Remap)) {
            SetError(outError, "Signal operationが不正です"); return false;
        }
        if ((signal.inputA >= 0 && !signalIds.contains(signal.inputA))
            || (signal.inputB >= 0 && !signalIds.contains(signal.inputB))) {
            SetError(outError, "Signal inputが存在しないnodeを参照しています"); return false;
        }
    }
    std::unordered_set<std::string> signalNames;
    for (const auto& output : asset.signalOutputs)
        if (output.name.empty() || !signalNames.insert(output.name).second
            || !signalIds.contains(output.nodeId)) {
            SetError(outError, "Signal outputが不正です"); return false;
        }
    const auto validSignalSource = [&](const VFXParamValue& value) {
        const auto* signal = std::get_if<VFXSignalRef>(&value.source);
        return signal == nullptr || signalNames.contains(signal->signalName);
    };
    for (const auto& parameter : asset.parameters)
        if (!validSignalSource(parameter.defaultValue)) {
            SetError(outError, "公開パラメーターが存在しないSignal outputを参照しています: " + parameter.name);
            return false;
        }
    for (const auto& variant : asset.variants)
        for (const auto& overrideValue : variant.overrides)
            if (!validSignalSource(overrideValue.value)) {
                SetError(outError, "Variantが存在しないSignal outputを参照しています: " + overrideValue.paramName);
                return false;
            }
    std::unordered_map<int, int> signalIndegree;
    std::unordered_map<int, std::vector<int>> signalAdjacency;
    for (const auto& signal : asset.signalNodes) {
        signalIndegree[signal.id] = (signal.inputA >= 0 ? 1 : 0) + (signal.inputB >= 0 ? 1 : 0);
        if (signal.inputA >= 0) signalAdjacency[signal.inputA].push_back(signal.id);
        if (signal.inputB >= 0) signalAdjacency[signal.inputB].push_back(signal.id);
    }
    std::queue<int> readySignals;
    for (const auto& [id, degree] : signalIndegree) if (degree == 0) readySignals.push(id);
    std::size_t processedSignals = 0;
    while (!readySignals.empty()) {
        const int id = readySignals.front(); readySignals.pop(); ++processedSignals;
        for (const int dependent : signalAdjacency[id])
            if (--signalIndegree[dependent] == 0) readySignals.push(dependent);
    }
    if (processedSignals != asset.signalNodes.size()) {
        SetError(outError, "Signal Graphに循環があります"); return false;
    }
    std::unordered_set<std::uint64_t> edges;
    int entryId = 0;
    for (const auto& node : asset.nodes)
        if (node.type == VFXNodeType::Entry) entryId = node.id;
    std::unordered_map<int, std::vector<int>> adjacency;
    for (const auto& link : asset.links) {
        if (!ids.contains(link.fromNode) || !ids.contains(link.toNode) || link.fromNode == link.toNode) {
            SetError(outError, "VFXリンクの参照先が無効です");
            return false;
        }
        const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(link.fromNode)) << 32)
            | static_cast<std::uint32_t>(link.toNode);
        if (!edges.insert(key).second) {
            SetError(outError, "VFXリンクが重複しています");
            return false;
        }
        if (link.toNode == entryId) {
            SetError(outError, "Entryノードへ入力リンクは接続できません");
            return false;
        }
        if (link.delay < 0.0f) {
            SetError(outError, "VFXリンクのDelayは0以上で指定してください");
            return false;
        }
        if (static_cast<int>(link.trigger) < 0
            || static_cast<int>(link.trigger) > static_cast<int>(VFXLinkTrigger::OnDeath)) {
            SetError(outError, "VFXリンクのTriggerが不正です");
            return false;
        }
        if ((link.trigger == VFXLinkTrigger::OnCollision || link.trigger == VFXLinkTrigger::OnDeath)
            && nodeTypes[link.fromNode] != VFXNodeType::Particle) {
            SetError(outError, "On Collision/On DeathリンクのsourceはParticleノードである必要があります");
            return false;
        }
        adjacency[link.fromNode].push_back(link.toNode);
    }
    std::unordered_set<int> reachable;
    std::vector<int> stack{ entryId };
    while (!stack.empty()) {
        const int id = stack.back();
        stack.pop_back();
        if (!reachable.insert(id).second) continue;
        for (const int next : adjacency[id]) stack.push_back(next);
    }
    if (reachable.size() != asset.nodes.size()) {
        SetError(outError, "Entryから到達できないVFXノードがあります");
        return false;
    }
    std::vector<float> starts;
    float duration = 0.0f;
    return BuildVFXGraphSchedule(asset, starts, duration, outError);
}

bool BuildVFXGraphSchedule(const VFXGraphAsset& asset,
                           std::vector<float>& outStartTimes,
                           float& outDuration,
                           std::string* outError)
{
    const std::size_t count = asset.nodes.size();
    std::unordered_map<int, std::size_t> indices;
    for (std::size_t i = 0; i < count; ++i) indices[asset.nodes[i].id] = i;
    struct ScheduledEdge {
        std::size_t target = 0;
        VFXLinkTrigger trigger = VFXLinkTrigger::OnComplete;
        float delay = 0.0f;
    };
    std::vector<std::vector<ScheduledEdge>> edges(count);
    std::vector<int> indegree(count, 0);
    for (const auto& link : asset.links) {
        const auto from = indices.find(link.fromNode);
        const auto to = indices.find(link.toNode);
        if (from == indices.end() || to == indices.end()) {
            SetError(outError, "VFXリンクの参照先が存在しません");
            return false;
        }
        edges[from->second].push_back({ to->second, link.trigger, link.delay });
        ++indegree[to->second];
    }
    std::queue<std::size_t> ready;
    for (std::size_t i = 0; i < count; ++i)
        if (indegree[i] == 0) ready.push(i);
    outStartTimes.assign(count, 0.0f);
    std::size_t processed = 0;
    while (!ready.empty()) {
        const std::size_t index = ready.front();
        ready.pop();
        ++processed;
        outStartTimes[index] += asset.nodes[index].startOffset;
        const float finish = outStartTimes[index] + asset.nodes[index].duration;
        for (const ScheduledEdge& edge : edges[index]) {
            // Collisionは実時刻を静的に決められないため、preview長の見積りでは完了イベントとして扱う。
            const float triggerTime = edge.trigger == VFXLinkTrigger::OnStart
                ? outStartTimes[index] : finish;
            outStartTimes[edge.target] = (std::max)(outStartTimes[edge.target], triggerTime + edge.delay);
            if (--indegree[edge.target] == 0) ready.push(edge.target);
        }
    }
    if (processed != count) {
        SetError(outError, "VFX Graphに循環リンクがあります");
        return false;
    }
    outDuration = 0.0f;
    for (std::size_t i = 0; i < count; ++i)
        outDuration = (std::max)(outDuration, outStartTimes[i] + asset.nodes[i].duration);
    return true;
}

bool SaveVFXGraphAsset(const std::string& path,
                       const VFXGraphAsset& asset,
                       std::string* outError)
{
    if (!ValidateVFXGraphAsset(asset, outError)) return false;
    toml::table root;
    root.insert("version", static_cast<std::int64_t>(asset.version));
    root.insert("name", asset.name);
    root.insert("maxParticles", static_cast<std::int64_t>(asset.maxParticles));
    root.insert("maxLights", static_cast<std::int64_t>(asset.maxLights));
    root.insert("maxAudioVoices", static_cast<std::int64_t>(asset.maxAudioVoices));
    toml::array nodes;
    for (const auto& node : asset.nodes) nodes.push_back(WriteNode(node));
    root.insert("nodes", std::move(nodes));
    toml::array links;
    for (const auto& link : asset.links) {
        toml::table table;
        table.insert("from", static_cast<std::int64_t>(link.fromNode));
        table.insert("to", static_cast<std::int64_t>(link.toNode));
        table.insert("trigger", static_cast<std::int64_t>(link.trigger));
        table.insert("delay", link.delay);
        links.push_back(std::move(table));
    }
    root.insert("links", std::move(links));
    toml::array parameters;
    for (const auto& parameter : asset.parameters) {
        toml::table table;
        table.insert("name", parameter.name);
        table.insert("type", static_cast<std::int64_t>(parameter.type));
        table.insert("minimum", parameter.minimum);
        table.insert("maximum", parameter.maximum);
        table.insert("hasRange", parameter.hasRange);
        table.insert("default", WriteParamValue(parameter.defaultValue));
        parameters.push_back(std::move(table));
    }
    root.insert("parameters", std::move(parameters));
    toml::array bindings;
    for (const auto& binding : asset.bindings) {
        toml::table table;
        table.insert("paramName", binding.paramName);
        table.insert("nodeId", static_cast<std::int64_t>(binding.nodeId));
        table.insert("schemaPath", binding.schemaPath);
        bindings.push_back(std::move(table));
    }
    root.insert("bindings", std::move(bindings));
    toml::array variants;
    for (const auto& variant : asset.variants) {
        toml::table table;
        table.insert("name", variant.name);
        toml::array overrides;
        for (const auto& overrideValue : variant.overrides) {
            toml::table item;
            item.insert("paramName", overrideValue.paramName);
            item.insert("value", WriteParamValue(overrideValue.value));
            overrides.push_back(std::move(item));
        }
        table.insert("overrides", std::move(overrides));
        variants.push_back(std::move(table));
    }
    root.insert("variants", std::move(variants));
    toml::array forwards;
    for (const auto& forward : asset.subGraphForwards) {
        toml::table table;
        table.insert("nodeId", static_cast<std::int64_t>(forward.nodeId));
        table.insert("parentParam", forward.parentParam);
        table.insert("childParam", forward.childParam);
        forwards.push_back(std::move(table));
    }
    root.insert("subGraphForwards", std::move(forwards));
    toml::array signalNodes;
    for (const auto& signal : asset.signalNodes) {
        toml::table table;
        table.insert("id", static_cast<std::int64_t>(signal.id));
        table.insert("operation", static_cast<std::int64_t>(signal.operation));
        table.insert("inputA", static_cast<std::int64_t>(signal.inputA));
        table.insert("inputB", static_cast<std::int64_t>(signal.inputB));
        table.insert("valueA", signal.valueA);
        table.insert("valueB", signal.valueB);
        signalNodes.push_back(std::move(table));
    }
    root.insert("signalNodes", std::move(signalNodes));
    toml::array signalOutputs;
    for (const auto& output : asset.signalOutputs) {
        toml::table table;
        table.insert("name", output.name);
        table.insert("nodeId", static_cast<std::int64_t>(output.nodeId));
        signalOutputs.push_back(std::move(table));
    }
    root.insert("signalOutputs", std::move(signalOutputs));
    EncodeGuidRefs(root);
    std::ostringstream stream;
    stream << root;
    if (!util::FileSystem::WriteText(AssetManager::ResolveAssetPath(path), stream.str())) {
        SetError(outError, "VFX Graphを書き込めませんでした");
        return false;
    }
    return true;
}

bool ParseVFXGraphAsset(const std::string& path,
                        VFXGraphAsset& outAsset,
                        std::string* outError)
{
    std::string text;
    if (!util::FileSystem::ReadText(AssetManager::ResolveAssetPath(path), text)) {
        SetError(outError, "VFX Graphを読み取れませんでした");
        return false;
    }
    toml::parse_result result = toml::parse(text);
    if (!result) {
        SetError(outError, "VFX GraphのTOMLが不正です");
        return false;
    }
    DecodeGuidRefs(result.table());
    VFXGraphAsset loaded;
    loaded.version = static_cast<int>(result["version"].value_or(std::int64_t{1}));
    loaded.version = (std::max)(loaded.version, 3);
    loaded.name = result["name"].value_or(std::string{ "VFX Graph" });
    loaded.maxParticles = static_cast<int>(result["maxParticles"].value_or(std::int64_t{100000}));
    loaded.maxLights = static_cast<int>(result["maxLights"].value_or(std::int64_t{8}));
    loaded.maxAudioVoices = static_cast<int>(result["maxAudioVoices"].value_or(std::int64_t{16}));
    if (const auto* nodes = result["nodes"].as_array()) {
        for (const auto& element : *nodes)
            if (const auto* table = element.as_table()) loaded.nodes.push_back(ReadNode(*table));
    }
    if (const auto* links = result["links"].as_array()) {
        for (const auto& element : *links) {
            const auto* table = element.as_table();
            if (table == nullptr) continue;
            loaded.links.push_back({
                static_cast<int>((*table)["from"].value_or(std::int64_t{0})),
                static_cast<int>((*table)["to"].value_or(std::int64_t{0})),
                static_cast<VFXLinkTrigger>((*table)["trigger"].value_or(std::int64_t{0})),
                static_cast<float>((*table)["delay"].value_or(0.0))
            });
        }
    }
    if (const auto* parameters = result["parameters"].as_array()) {
        for (const auto& element : *parameters) {
            const auto* table = element.as_table();
            if (table == nullptr) continue;
            VFXParamDefinition parameter;
            parameter.name = (*table)["name"].value_or(std::string{});
            parameter.type = static_cast<VFXParamType>((*table)["type"].value_or(std::int64_t{0}));
            parameter.minimum = static_cast<float>((*table)["minimum"].value_or(0.0));
            parameter.maximum = static_cast<float>((*table)["maximum"].value_or(1.0));
            parameter.hasRange = (*table)["hasRange"].value_or(false);
            if (const auto* value = (*table)["default"].as_table()) parameter.defaultValue = ReadParamValue(*value);
            loaded.parameters.push_back(std::move(parameter));
        }
    }
    if (const auto* bindings = result["bindings"].as_array()) {
        for (const auto& element : *bindings) {
            const auto* table = element.as_table();
            if (table == nullptr) continue;
            loaded.bindings.push_back({
                (*table)["paramName"].value_or(std::string{}),
                static_cast<int>((*table)["nodeId"].value_or(std::int64_t{0})),
                (*table)["schemaPath"].value_or(std::string{}) });
        }
    }
    if (const auto* variants = result["variants"].as_array()) {
        for (const auto& element : *variants) {
            const auto* table = element.as_table();
            if (table == nullptr) continue;
            VFXVariantSet variant;
            variant.name = (*table)["name"].value_or(std::string{});
            if (const auto* overrides = (*table)["overrides"].as_array()) {
                for (const auto& overrideElement : *overrides) {
                    const auto* item = overrideElement.as_table();
                    if (item == nullptr) continue;
                    VFXParamOverride overrideValue;
                    overrideValue.paramName = (*item)["paramName"].value_or(std::string{});
                    if (const auto* value = (*item)["value"].as_table()) overrideValue.value = ReadParamValue(*value);
                    variant.overrides.push_back(std::move(overrideValue));
                }
            }
            loaded.variants.push_back(std::move(variant));
        }
    }
    if (const auto* forwards = result["subGraphForwards"].as_array()) {
        for (const auto& element : *forwards) {
            const auto* table = element.as_table();
            if (table == nullptr) continue;
            loaded.subGraphForwards.push_back({
                static_cast<int>((*table)["nodeId"].value_or(std::int64_t{0})),
                (*table)["parentParam"].value_or(std::string{}),
                (*table)["childParam"].value_or(std::string{}) });
        }
    }
    if (const auto* signals = result["signalNodes"].as_array()) {
        for (const auto& element : *signals) {
            const auto* table = element.as_table();
            if (table == nullptr) continue;
            loaded.signalNodes.push_back({
                static_cast<int>((*table)["id"].value_or(std::int64_t{0})),
                static_cast<VFXSignalOperation>((*table)["operation"].value_or(std::int64_t{0})),
                static_cast<int>((*table)["inputA"].value_or(std::int64_t{-1})),
                static_cast<int>((*table)["inputB"].value_or(std::int64_t{-1})),
                static_cast<float>((*table)["valueA"].value_or(0.0)),
                static_cast<float>((*table)["valueB"].value_or(1.0)) });
        }
    }
    if (const auto* outputs = result["signalOutputs"].as_array()) {
        for (const auto& element : *outputs) {
            const auto* table = element.as_table();
            if (table == nullptr) continue;
            loaded.signalOutputs.push_back({ (*table)["name"].value_or(std::string{}),
                static_cast<int>((*table)["nodeId"].value_or(std::int64_t{0})) });
        }
    }
    outAsset = std::move(loaded);
    return true;
}

bool LoadVFXGraphAsset(const std::string& path,
                       VFXGraphAsset& outAsset,
                       std::string* outError)
{
    VFXGraphAsset loaded;
    if (!ParseVFXGraphAsset(path, loaded, outError)) return false;
    if (!ValidateVFXGraphAsset(loaded, outError)) return false;
    outAsset = std::move(loaded);
    return true;
}

} // namespace fbzz::asset
