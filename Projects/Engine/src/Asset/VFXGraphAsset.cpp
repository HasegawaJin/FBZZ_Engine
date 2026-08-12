// FBZZ Engine
// VFXGraphAsset.cpp | fbzz::asset
// .vfxグラフのTOML入出力、参照GUID変換、DAG検証と時間スケジュール構築
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/VFXAuthoringSchema.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <any>
#include <cmath>
#include <filesystem>
#include <queue>
#include <system_error>
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
        const std::uint32_t count = (std::min)(curve->curve.keyCount, scene::kMaxParticleCurveKeys);
        for (std::uint32_t index = 0; index < count; ++index)
            keys.push_back(toml::array{ curve->curve.keys[index].time, curve->curve.keys[index].value });
        table.insert("keys", std::move(keys));
    } else if (const auto* gradient = std::get_if<VFXGradientSource>(&value.source)) {
        toml::array keys;
        const std::uint32_t count = (std::min)(gradient->gradient.keyCount, scene::kMaxParticleCurveKeys);
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
            curve.curve.keyCount = static_cast<std::uint32_t>(
                (std::min)(keys->size(), curve.curve.keys.size()));
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
            gradient.gradient.keyCount = static_cast<std::uint32_t>(
                (std::min)(keys->size(), gradient.gradient.keys.size()));
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
    table.insert("enabled", node.enabled);
    table.insert("editorX", node.editorX);
    table.insert("editorY", node.editorY);
    table.insert("startOffset", node.startOffset);
    table.insert("duration", node.duration);
    table.insert("position", WriteVector3(node.localPosition));
    table.insert("rotation", WriteVector3(node.localRotationDegrees));
    table.insert("scale", WriteVector3(node.localScale));
    table.insert("attachBone", node.attachBone);
    table.insert("parentNodeId", static_cast<std::int64_t>(node.parentNodeId));

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
    light.insert("useIntensityCurve", node.light.useIntensityCurve);
    light.insert("intensityCurve", SerializeParticleCurve(node.light.intensityCurve));
    light.insert("useColorGradient", node.light.useColorGradient);
    light.insert("colorGradient", SerializeParticleGradient(node.light.colorGradient));
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
    decal.insert("angleFadeStrength", node.decal.angleFadeStrength);
    decal.insert("angleFadeDegrees", node.decal.angleFadeDegrees);
    decal.insert("useFadeCurve", node.decal.useFadeCurve);
    decal.insert("fadeCurve", SerializeParticleCurve(node.decal.fadeCurve));
    table.insert("decal", std::move(decal));
    toml::table subGraph;
    subGraph.insert("graphPath", node.subGraph.graphPath);
    table.insert("subGraph", std::move(subGraph));

    toml::table forceField;
    forceField.insert("fieldType", static_cast<std::int64_t>(node.forceField.fieldType));
    forceField.insert("strength", node.forceField.strength);
    forceField.insert("radius", node.forceField.radius);
    forceField.insert("falloffPower", node.forceField.falloffPower);
    forceField.insert("direction", WriteVector3(node.forceField.direction));
    forceField.insert("noiseFrequency", node.forceField.noiseFrequency);
    forceField.insert("noiseSpeed", node.forceField.noiseSpeed);
    table.insert("forceField", std::move(forceField));

    toml::table mesh;
    mesh.insert("meshPath", node.mesh.meshPath);
    mesh.insert("materialPath", node.mesh.materialPath);
    mesh.insert("scaleStart", node.mesh.scaleStart);
    mesh.insert("scaleEnd", node.mesh.scaleEnd);
    mesh.insert("scaleEasePower", node.mesh.scaleEasePower);
    mesh.insert("colorStart", WriteVector4(node.mesh.colorStart));
    mesh.insert("colorEnd", WriteVector4(node.mesh.colorEnd));
    mesh.insert("animatedParam", node.mesh.animatedParam);
    mesh.insert("paramStart", node.mesh.paramStart);
    mesh.insert("paramEnd", node.mesh.paramEnd);
    table.insert("mesh", std::move(mesh));

    toml::table animatedMesh;
    animatedMesh.insert("modelPath", node.animatedMesh.modelPath);
    animatedMesh.insert("controllerPath", node.animatedMesh.controllerPath);
    animatedMesh.insert("materialPath", node.animatedMesh.materialPath);
    animatedMesh.insert("initialState", node.animatedMesh.initialState);
    animatedMesh.insert("speed", node.animatedMesh.speed);
    animatedMesh.insert("startNormalizedTime", node.animatedMesh.startNormalizedTime);
    animatedMesh.insert("meshIndex", static_cast<std::int64_t>(node.animatedMesh.meshIndex));
    animatedMesh.insert("loop", node.animatedMesh.loop);
    animatedMesh.insert("syncToGraphTime", node.animatedMesh.syncToGraphTime);
    animatedMesh.insert("applyRootMotion", node.animatedMesh.applyRootMotion);
    table.insert("animatedMesh", std::move(animatedMesh));

    toml::table screenEffect;
    screenEffect.insert("flashColor", WriteVector3(node.screenEffect.flashColor));
    screenEffect.insert("flashIntensity", node.screenEffect.flashIntensity);
    screenEffect.insert("bloomBoost", node.screenEffect.bloomBoost);
    screenEffect.insert("chromaticAberration", node.screenEffect.chromaticAberration);
    screenEffect.insert("lensDistortion", node.screenEffect.lensDistortion);
    screenEffect.insert("vignette", node.screenEffect.vignette);
    screenEffect.insert("fadeInTime", node.screenEffect.fadeInTime);
    screenEffect.insert("fadeOutTime", node.screenEffect.fadeOutTime);
    table.insert("screenEffect", std::move(screenEffect));

    toml::table cameraShake;
    cameraShake.insert("amplitude", node.cameraShake.amplitude);
    cameraShake.insert("rotationAmplitude", node.cameraShake.rotationAmplitude);
    cameraShake.insert("frequency", node.cameraShake.frequency);
    cameraShake.insert("falloffPower", node.cameraShake.falloffPower);
    cameraShake.insert("radius", node.cameraShake.radius);
    table.insert("cameraShake", std::move(cameraShake));

    toml::table timeScale;
    timeScale.insert("timeScale", node.timeScale.timeScale);
    timeScale.insert("blendInTime", node.timeScale.blendInTime);
    timeScale.insert("blendOutTime", node.timeScale.blendOutTime);
    table.insert("timeScale", std::move(timeScale));

    toml::table wind;
    wind.insert("direction", WriteVector3(node.wind.direction));
    wind.insert("strength", node.wind.strength);
    wind.insert("turbulence", node.wind.turbulence);
    wind.insert("pulseFrequency", node.wind.pulseFrequency);
    table.insert("wind", std::move(wind));
    return table;
}

VFXGraphNode ReadNode(const toml::table& table)
{
    VFXGraphNode node;
    node.id = static_cast<int>(table["id"].value_or(std::int64_t{0}));
    node.type = static_cast<VFXNodeType>(table["type"].value_or(std::int64_t{2}));
    node.name = table["name"].value_or(std::string{ "Effect" });
    node.enabled = table["enabled"].value_or(true);
    node.editorX = static_cast<float>(table["editorX"].value_or(0.0));
    node.editorY = static_cast<float>(table["editorY"].value_or(0.0));
    node.startOffset = static_cast<float>(table["startOffset"].value_or(0.0));
    node.duration = static_cast<float>(table["duration"].value_or(1.0));
    node.localPosition = ReadVector3(table["position"], math::Vector3::ZERO);
    node.localRotationDegrees = ReadVector3(table["rotation"], math::Vector3::ZERO);
    node.localScale = ReadVector3(table["scale"], math::Vector3::ONE);
    node.attachBone = table["attachBone"].value_or(std::string{});
    // 既存 .vfx には無いキー。欠けていれば -1 = owner 直下で、従来と同じフラット構成になる。
    node.parentNodeId = static_cast<int>(table["parentNodeId"].value_or(std::int64_t{ -1 }));

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
        node.light.useIntensityCurve = (*value)["useIntensityCurve"].value_or(false);
        DeserializeParticleCurve(*value, "intensityCurve", node.light.intensityCurve);
        node.light.useColorGradient = (*value)["useColorGradient"].value_or(false);
        DeserializeParticleGradient(*value, "colorGradient", node.light.colorGradient);
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
        node.decal.angleFadeStrength = static_cast<float>((*value)["angleFadeStrength"].value_or(1.0));
        node.decal.angleFadeDegrees = static_cast<float>((*value)["angleFadeDegrees"].value_or(70.0));
        node.decal.useFadeCurve = (*value)["useFadeCurve"].value_or(false);
        DeserializeParticleCurve(*value, "fadeCurve", node.decal.fadeCurve);
    }
    if (const auto* value = table["subGraph"].as_table())
        node.subGraph.graphPath = (*value)["graphPath"].value_or(std::string{});
    if (const auto* value = table["forceField"].as_table()) {
        node.forceField.fieldType = static_cast<int>((*value)["fieldType"].value_or(std::int64_t{0}));
        node.forceField.strength = static_cast<float>((*value)["strength"].value_or(5.0));
        node.forceField.radius = static_cast<float>((*value)["radius"].value_or(5.0));
        node.forceField.falloffPower = static_cast<float>((*value)["falloffPower"].value_or(2.0));
        node.forceField.direction = ReadVector3((*value)["direction"], node.forceField.direction);
        node.forceField.noiseFrequency = static_cast<float>((*value)["noiseFrequency"].value_or(0.5));
        node.forceField.noiseSpeed = static_cast<float>((*value)["noiseSpeed"].value_or(1.0));
    }
    if (const auto* value = table["mesh"].as_table()) {
        node.mesh.meshPath = (*value)["meshPath"].value_or(node.mesh.meshPath);
        node.mesh.materialPath = (*value)["materialPath"].value_or(std::string{});
        node.mesh.scaleStart = static_cast<float>((*value)["scaleStart"].value_or(0.1));
        node.mesh.scaleEnd = static_cast<float>((*value)["scaleEnd"].value_or(4.0));
        node.mesh.scaleEasePower = static_cast<float>((*value)["scaleEasePower"].value_or(0.45));
        node.mesh.colorStart = ReadVector4((*value)["colorStart"], node.mesh.colorStart);
        node.mesh.colorEnd = ReadVector4((*value)["colorEnd"], node.mesh.colorEnd);
        node.mesh.animatedParam = (*value)["animatedParam"].value_or(node.mesh.animatedParam);
        node.mesh.paramStart = static_cast<float>((*value)["paramStart"].value_or(0.0));
        node.mesh.paramEnd = static_cast<float>((*value)["paramEnd"].value_or(1.0));
    }
    if (const auto* value = table["animatedMesh"].as_table()) {
        node.animatedMesh.modelPath = (*value)["modelPath"].value_or(std::string{});
        node.animatedMesh.controllerPath = (*value)["controllerPath"].value_or(std::string{});
        node.animatedMesh.materialPath = (*value)["materialPath"].value_or(std::string{});
        node.animatedMesh.initialState = (*value)["initialState"].value_or(std::string{});
        node.animatedMesh.speed = static_cast<float>((*value)["speed"].value_or(1.0));
        node.animatedMesh.startNormalizedTime =
            static_cast<float>((*value)["startNormalizedTime"].value_or(0.0));
        node.animatedMesh.meshIndex =
            static_cast<int>((*value)["meshIndex"].value_or(std::int64_t{-1}));
        node.animatedMesh.loop = (*value)["loop"].value_or(false);
        node.animatedMesh.syncToGraphTime = (*value)["syncToGraphTime"].value_or(true);
        node.animatedMesh.applyRootMotion = (*value)["applyRootMotion"].value_or(false);
    }
    if (const auto* value = table["screenEffect"].as_table()) {
        node.screenEffect.flashColor = ReadVector3((*value)["flashColor"], node.screenEffect.flashColor);
        node.screenEffect.flashIntensity = static_cast<float>((*value)["flashIntensity"].value_or(0.0));
        node.screenEffect.bloomBoost = static_cast<float>((*value)["bloomBoost"].value_or(0.0));
        node.screenEffect.chromaticAberration =
            static_cast<float>((*value)["chromaticAberration"].value_or(0.0));
        node.screenEffect.lensDistortion = static_cast<float>((*value)["lensDistortion"].value_or(0.0));
        node.screenEffect.vignette = static_cast<float>((*value)["vignette"].value_or(0.0));
        node.screenEffect.fadeInTime = static_cast<float>((*value)["fadeInTime"].value_or(0.03));
        node.screenEffect.fadeOutTime = static_cast<float>((*value)["fadeOutTime"].value_or(0.25));
    }
    if (const auto* value = table["cameraShake"].as_table()) {
        node.cameraShake.amplitude = static_cast<float>((*value)["amplitude"].value_or(0.12));
        node.cameraShake.rotationAmplitude =
            static_cast<float>((*value)["rotationAmplitude"].value_or(1.2));
        node.cameraShake.frequency = static_cast<float>((*value)["frequency"].value_or(22.0));
        node.cameraShake.falloffPower = static_cast<float>((*value)["falloffPower"].value_or(2.0));
        node.cameraShake.radius = static_cast<float>((*value)["radius"].value_or(25.0));
    }
    if (const auto* value = table["timeScale"].as_table()) {
        node.timeScale.timeScale = static_cast<float>((*value)["timeScale"].value_or(0.15));
        node.timeScale.blendInTime = static_cast<float>((*value)["blendInTime"].value_or(0.0));
        node.timeScale.blendOutTime = static_cast<float>((*value)["blendOutTime"].value_or(0.12));
    }
    if (const auto* value = table["wind"].as_table()) {
        node.wind.direction = ReadVector3((*value)["direction"], node.wind.direction);
        node.wind.strength = static_cast<float>((*value)["strength"].value_or(3.0));
        node.wind.turbulence = static_cast<float>((*value)["turbulence"].value_or(0.3));
        node.wind.pulseFrequency = static_cast<float>((*value)["pulseFrequency"].value_or(0.5));
    }
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
    case VFXNodeType::ForceField: return "Force Field";
    case VFXNodeType::Mesh: return "Mesh";
    case VFXNodeType::ScreenEffect: return "Screen Effect";
    case VFXNodeType::CameraShake: return "Camera Shake";
    case VFXNodeType::TimeScale: return "Time Scale";
    case VFXNodeType::Wind: return "Wind";
    case VFXNodeType::Reroute: return "Reroute";
    case VFXNodeType::AnimatedMesh: return "Animated Mesh";
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
    case VFXLinkTrigger::OnAnimationEvent: return "On Animation Event";
    case VFXLinkTrigger::OnTrigger: return "On Trigger";
    }
    return "Unknown";
}

VFXGraphBudgetStats CalculateVFXGraphBudget(const VFXGraphAsset& asset)
{
    VFXGraphBudgetStats stats;
    for (const auto& node : asset.nodes) {
        if (!node.enabled) continue;
        if (node.type == VFXNodeType::Particle)
            stats.particles += (std::max)(node.particle.maxParticles, 0);
        else if (node.type == VFXNodeType::Light)
            ++stats.lights;
        else if (node.type == VFXNodeType::Audio)
            ++stats.audioVoices;
    }
    return stats;
}

std::vector<std::string> CollectVFXGraphDependencies(const VFXGraphAsset& asset)
{
    // WHY: Build時にEditorの現在選択へ依存せず、VFX単体から必要なモデル・Clip・Materialを
    //      追跡できるよう、Controller内部のMotion参照まで展開して安定順で保存する。
    std::unordered_set<std::string> unique;
    const auto add = [&unique](const std::string& path) {
        if (!path.empty()) unique.insert(path);
    };
    const auto addMotion = [&add](const scene::BlendTreeMotion& motion) {
        add(motion.sourcePath);
    };

    for (const auto& node : asset.nodes) {
        add(node.particle.texturePath);
        add(node.particle.materialPath);
        add(node.particle.meshShapePath);
        add(node.particle.meshParticlePath);
        add(node.particle.motionVectorTexturePath);
        add(node.trail.meshPath);
        add(node.trail.materialPath);
        add(node.trail.texturePath);
        add(node.audio.clipPath);
        add(node.decal.albedoPath);
        add(node.decal.normalPath);
        add(node.decal.emissivePath);
        add(node.subGraph.graphPath);
        add(node.mesh.meshPath);
        add(node.mesh.materialPath);
        add(node.animatedMesh.modelPath);
        add(node.animatedMesh.controllerPath);
        add(node.animatedMesh.materialPath);

        if (node.type != VFXNodeType::AnimatedMesh
            || node.animatedMesh.controllerPath.empty()) continue;
        AnimatorControllerAsset controller;
        if (!LoadAnimatorControllerAsset(node.animatedMesh.controllerPath, controller)) continue;
        for (const auto& clipSource : controller.clipSources) add(clipSource);
        for (const auto& state : controller.states) {
            add(state.sourcePath);
            for (const auto& motion : state.blendTree1D.motions) addMotion(motion);
            for (const auto& motion : state.blendTree2D.motions) addMotion(motion);
        }
    }

    std::vector<std::string> result(unique.begin(), unique.end());
    std::sort(result.begin(), result.end());
    return result;
}

float ResolveVFXThumbnailTime(const VFXGraphAsset& asset)
{
    if (asset.thumbnailTime >= 0.0f) return asset.thumbnailTime;
    // 自動時刻。全長の 25% は「立ち上がりが終わり、まだ消え際に入っていない」区間で、
    // 爆発でも煙でも一枚絵として最も内容が読める。スケジュールが組めない壊れた
    // グラフでは 0 を返す (何も出ないより t=0 の絵の方が原因を示唆できる)。
    std::vector<float> starts;
    float duration = 0.0f;
    if (!BuildVFXGraphSchedule(asset, starts, duration, nullptr) || duration <= 0.0f)
        return 0.0f;
    return duration * 0.25f;
}

std::vector<std::string> CollectMissingVFXReferences(const VFXGraphAsset& asset)
{
    // 判定は CollectVFXGraphWarnings の MISSING_ASSET が唯一の正本。ここで
    // 独自にパスを検査すると、警告 banner と Template 適用前チェックで
    // 「片方だけ不足と言う」食い違いが必ず生まれる。
    std::unordered_set<std::string> unique;
    for (const auto& warning : CollectVFXGraphWarnings(asset, true)) {
        if (warning.code != "MISSING_ASSET") continue;
        // 本文末尾の " -> <path>" を優先して拾う。GUID 切れには矢印が無いため
        // その場合はメッセージ全体を返し、少なくともノード名が判るようにする。
        const std::size_t arrow = warning.message.rfind(" -> ");
        unique.insert(arrow == std::string::npos ? warning.message
                                                 : warning.message.substr(arrow + 4));
    }
    std::vector<std::string> result(unique.begin(), unique.end());
    std::sort(result.begin(), result.end());
    return result;
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
        if (node.type == VFXNodeType::AnimatedMesh
            && (node.animatedMesh.modelPath.empty()
                || node.animatedMesh.controllerPath.empty())) {
            SetError(outError,
                "Animated MeshノードにはModelとAnimator Controllerが必要です");
            return false;
        }
        if (node.type == VFXNodeType::AnimatedMesh) {
            AnimatorControllerAsset controller;
            if (!LoadAnimatorControllerAsset(node.animatedMesh.controllerPath, controller)) {
                SetError(outError,
                    "Animated MeshノードのAnimator Controllerを読み込めません: "
                    + node.animatedMesh.controllerPath);
                return false;
            }
        }
    }
    if (entryCount != 1) {
        SetError(outError, "VFX GraphにはEntryノードが1個必要です");
        return false;
    }
    // Transform 親子 (parentNodeId) の健全性。link の DAG とは独立した木なので別に検証する。
    // WHY: 未知 id や循環を許すと、ランタイムの SetParent が無限ループするか
    //      親のいない浮いた GameObject を作る。どちらも実行してからでないと気づけないため、
    //      保存前に止める (「見た目が変」ではなく「構造が壊れている」種類の誤りとして扱う)。
    {
        std::unordered_map<int, int> parentOf;
        for (const auto& node : asset.nodes) {
            if (node.parentNodeId == -1) continue;
            if (node.parentNodeId == node.id) {
                SetError(outError, "VFXノードの親に自分自身を指定できません");
                return false;
            }
            if (!ids.contains(node.parentNodeId)) {
                SetError(outError, "VFXノードの親に存在しないノードが指定されています");
                return false;
            }
            parentOf[node.id] = node.parentNodeId;
        }
        // 各ノードから根へ辿る。ノード数を超えて登れたら必ず循環している。
        const std::size_t limit = asset.nodes.size() + 1;
        for (const auto& [child, parent] : parentOf) {
            int cursor = parent;
            for (std::size_t step = 0; cursor != -1; ++step) {
                if (cursor == child || step > limit) {
                    SetError(outError, "VFXノードの親子関係が循環しています");
                    return false;
                }
                const auto it = parentOf.find(cursor);
                cursor = (it != parentOf.end()) ? it->second : -1;
            }
        }
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
            || static_cast<int>(link.trigger) > static_cast<int>(VFXLinkTrigger::OnTrigger)) {
            SetError(outError, "VFXリンクのTriggerが不正です");
            return false;
        }
        if ((link.trigger == VFXLinkTrigger::OnCollision || link.trigger == VFXLinkTrigger::OnDeath)
            && nodeTypes[link.fromNode] != VFXNodeType::Particle) {
            SetError(outError, "On Collision/On DeathリンクのsourceはParticleノードである必要があります");
            return false;
        }
        if (link.trigger == VFXLinkTrigger::OnAnimationEvent
            && nodeTypes[link.fromNode] != VFXNodeType::AnimatedMesh) {
            SetError(outError,
                "On Animation EventリンクのsourceはAnimated Meshノードである必要があります");
            return false;
        }
        if (link.trigger == VFXLinkTrigger::OnTrigger && link.eventName.empty()) {
            SetError(outError, "On TriggerリンクにはTrigger Nameが必要です");
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

namespace {

// 公開パラメーターの定数 default を、bind 先 leaf の型に合わせた std::any へ落とす。
// EvaluateVFXParamValue の Constant 分岐と同じ変換にしてあり、
// 「実際に適用される値」と同じものを比較できるようにしている。
bool ConstantToAny(const VFXConstant& constant, reflection::PropertyType targetType,
                   std::any& output)
{
    if (const auto* item = std::get_if<float>(&constant)) output = *item;
    else if (const auto* item = std::get_if<int>(&constant)) output = *item;
    else if (const auto* item = std::get_if<bool>(&constant)) output = *item;
    else if (const auto* item = std::get_if<math::Vector4>(&constant)) {
        output = targetType == reflection::PropertyType::Vector3
            ? std::any{ math::Vector3{ item->x, item->y, item->z } } : std::any{ *item };
    } else if (const auto* item = std::get_if<math::Vector3>(&constant)) output = *item;
    else if (const auto* item = std::get_if<std::string>(&constant)) output = *item;
    return output.has_value();
}

// float 比較は相対誤差を許す。TOML の往復で末尾ビットが揺れるだけの差で
// 警告を出すと、警告そのものが無視されるようになるため。
bool NearlyEqual(float a, float b)
{
    const float scale = (std::max)({ 1.0f, std::fabs(a), std::fabs(b) });
    return std::fabs(a - b) <= 0.0005f * scale;
}

bool SchemaValueEquals(reflection::PropertyType type, const std::any& a, const std::any& b)
{
    switch (type) {
    case reflection::PropertyType::Float:
        return NearlyEqual(std::any_cast<float>(a), std::any_cast<float>(b));
    case reflection::PropertyType::Int:
    case reflection::PropertyType::Enum:
        return std::any_cast<int>(a) == std::any_cast<int>(b);
    case reflection::PropertyType::Bool:
        return std::any_cast<bool>(a) == std::any_cast<bool>(b);
    case reflection::PropertyType::Vector3: {
        const auto& x = std::any_cast<const math::Vector3&>(a);
        const auto& y = std::any_cast<const math::Vector3&>(b);
        return NearlyEqual(x.x, y.x) && NearlyEqual(x.y, y.y) && NearlyEqual(x.z, y.z);
    }
    case reflection::PropertyType::Color: {
        const auto& x = std::any_cast<const math::Vector4&>(a);
        const auto& y = std::any_cast<const math::Vector4&>(b);
        return NearlyEqual(x.x, y.x) && NearlyEqual(x.y, y.y)
            && NearlyEqual(x.z, y.z) && NearlyEqual(x.w, y.w);
    }
    case reflection::PropertyType::String:
    case reflection::PropertyType::AssetRef:
        return std::any_cast<const std::string&>(a) == std::any_cast<const std::string&>(b);
    default:
        // Curve/Gradient/Struct/Array は等価判定を持たない。値ソースが定数でない場合も
        // ここへ来るが、その場合そもそも「default で上書きされる」警告の対象外。
        return true;
    }
}

} // namespace

std::vector<VFXGraphWarning> CollectVFXGraphWarnings(const VFXGraphAsset& asset,
                                                     bool checkAssetReferences)
{
    std::vector<VFXGraphWarning> warnings;
    const auto add = [&](int nodeId, const char* code, std::string message) {
        warnings.push_back({ nodeId, code, std::move(message) });
    };

    // 参照切れの検出。
    // WHY: テクスチャが見つからないエフェクトは「真っ白」「不可視」という形でしか
    //      現れないため、パラメーターをいくら触っても直らない。原因がパスであることに
    //      気付くまでの時間が最も無駄なので、値の警告より先に出す。
    const auto checkAsset = [&](int nodeId, const std::string& name,
                                const char* label, const std::string& reference) {
        // 空 = 未使用。primitive: は組み込み形状で実ファイルを持たない。
        // guid: は解決済みのはずだが、参照先が消えていると decode されず残る。
        if (reference.empty() || reference.starts_with("primitive:")) return;
        if (reference.starts_with("guid:")) {
            add(nodeId, "MISSING_ASSET",
                name + ": " + label + " の参照先アセットが見つかりません (削除済み GUID)");
            return;
        }
        const std::string resolved = AssetManager::ResolveAssetPath(reference);
        std::error_code code;
        if (resolved.empty() || !std::filesystem::exists(resolved, code))
            add(nodeId, "MISSING_ASSET",
                name + ": " + label + " が見つかりません -> " + reference);
    };
    if (checkAssetReferences) for (const auto& node : asset.nodes) {
        if (node.type == VFXNodeType::Particle) {
            checkAsset(node.id, node.name, "Texture", node.particle.texturePath);
            checkAsset(node.id, node.name, "Material", node.particle.materialPath);
            checkAsset(node.id, node.name, "Mesh Shape", node.particle.meshShapePath);
            checkAsset(node.id, node.name, "Mesh Particle", node.particle.meshParticlePath);
            checkAsset(node.id, node.name, "Motion Vector", node.particle.motionVectorTexturePath);
        } else if (node.type == VFXNodeType::Trail || node.type == VFXNodeType::MeshTrail) {
            checkAsset(node.id, node.name, "Texture", node.trail.texturePath);
            checkAsset(node.id, node.name, "Material", node.trail.materialPath);
        } else if (node.type == VFXNodeType::SubGraph) {
            checkAsset(node.id, node.name, "Sub Graph", node.subGraph.graphPath);
        } else if (node.type == VFXNodeType::AnimatedMesh) {
            checkAsset(node.id, node.name, "Model", node.animatedMesh.modelPath);
            checkAsset(node.id, node.name, "Animator Controller",
                       node.animatedMesh.controllerPath);
            checkAsset(node.id, node.name, "Material", node.animatedMesh.materialPath);
        }
    }

    // Transform 親子の縮退。構造は壊れていないので保存は通すが、指定した意図は実行時に消える。
    // WHY: Entry / Delay は時間だけを表すノードで GameObject を作らない。そこを親に選ぶと
    //      黙って owner 直下へ落ちるため、「親を設定したのに一緒に動かない」となる。
    //      同じく attachBone は socket 追従が優先されるため、親指定と併用すると片方が無視される。
    for (const auto& node : asset.nodes) {
        if (node.parentNodeId == -1) continue;
        const auto parent = std::find_if(asset.nodes.begin(), asset.nodes.end(),
            [&](const VFXGraphNode& candidate) { return candidate.id == node.parentNodeId; });
        if (parent == asset.nodes.end()) continue; // 未知 id は Validate 側のエラー
        if (VFXNodeHasNoInstance(parent->type))
            add(node.id, "PARENT_HAS_NO_TRANSFORM",
                node.name + ": 親に指定した \"" + parent->name + "\" は "
                + VFXNodeTypeName(parent->type)
                + " なので実体を持ちません。実行時は owner 直下へ落ちます");
        else if (!node.attachBone.empty())
            add(node.id, "PARENT_OVERRIDDEN_BY_SOCKET",
                node.name + ": Bone / Socket 指定があるため Parent Node は無視されます");
    }

    for (const auto& node : asset.nodes) {
        if (node.type != VFXNodeType::Particle) continue;
        const auto& p = node.particle;

        // Particle.hlsl は「回転させた corner に軸倍率を掛ける」順序なので、
        // 回転と非等方サイズを同時に使うとスプライトが平行四辺形へせん断される。
        // どちらか一方に寄せるのが正解で、両立させる方法は無い。
        const bool spins = p.angularVelocityMin != 0.0f || p.angularVelocityMax != 0.0f
                        || p.useRotationCurve;
        const bool anisotropic = !NearlyEqual(p.sizeAxisScale.x, p.sizeAxisScale.y);
        if (spins && anisotropic)
            add(node.id, "SHEARED_SPRITE",
                node.name + ": 回転 (Angular Velocity / Rotation Curve) と非等方 Size Axis Scale "
                            "を同時に使うとスプライトがせん断されます。どちらか一方にしてください");

        // saturate されるため 1.0 超はすべて同じ挙動 (= 元の色を捨てる) になる。
        if (p.sixWayLighting && p.lightingStrength > 1.0f)
            add(node.id, "LIGHTING_SATURATED",
                node.name + ": Lighting Strength が 1.0 を超えています "
                            "(シェーダー側で 1.0 に丸められ、暗い環境では煙が黒く潰れます)");

        // 半透明の重なりは描画順で結果が変わる。加算は順序非依存なので対象外。
        if (p.blendMode != scene::ParticleBlendMode::Additive
            && p.sortMode == scene::ParticleSortMode::None && p.maxParticles > 1)
            add(node.id, "ALPHA_NO_SORT",
                node.name + ": Alpha / Premultiplied ブレンドで Sort Mode が None です "
                            "(粒子の前後関係がフレームごとに入れ替わります)");
    }

    // bind 済み leaf は生成時に必ず公開パラメーターの値で上書きされる。
    // ノード側の値と食い違っていると「Inspector で直したのに反映されない」になる。
    for (const auto& binding : asset.bindings) {
        const auto node = std::find_if(asset.nodes.begin(), asset.nodes.end(),
            [&](const VFXGraphNode& value) { return value.id == binding.nodeId; });
        if (node == asset.nodes.end()) continue;
        const auto parameter = std::find_if(asset.parameters.begin(), asset.parameters.end(),
            [&](const VFXParamDefinition& value) { return value.name == binding.paramName; });
        if (parameter == asset.parameters.end()) continue;
        const auto* constant = std::get_if<VFXConstant>(&parameter->defaultValue.source);
        if (constant == nullptr) continue; // Curve/Random/Signal は毎回評価されるため比較対象外

        reflection::ResolvedProperty resolved;
        if (!reflection::ResolveProperty(GetVFXNodeSchema(), &*node, binding.schemaPath, resolved)
            || resolved.property == nullptr || resolved.property->get == nullptr) continue;
        std::any defaultValue;
        if (!ConstantToAny(*constant, resolved.property->type, defaultValue)) continue;
        const std::any nodeValue = resolved.property->get(resolved.constOwner);
        if (nodeValue.type() != defaultValue.type()) continue;
        if (!SchemaValueEquals(resolved.property->type, nodeValue, defaultValue))
            add(node->id, "BOUND_FIELD_OVERRIDDEN",
                node->name + "." + binding.schemaPath
                    + ": 公開パラメーター \"" + binding.paramName
                    + "\" の default で上書きされるため、ノード側の値は使われません");
    }
    return warnings;
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
        // Reroute は配線を折り曲げるためだけのノードで、時間を一切消費しない。
        // WHY: ここで startOffset / duration を効かせると、「線を整理しただけ」のはずの
        //      操作でエフェクトのタイミングが変わる。見た目の整理と実行の意味を分ける。
        const bool passthrough = VFXNodeIsPassthrough(asset.nodes[index].type);
        if (!passthrough) outStartTimes[index] += asset.nodes[index].startOffset;
        const float finish = passthrough
            ? outStartTimes[index] : outStartTimes[index] + asset.nodes[index].duration;
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
    for (std::size_t i = 0; i < count; ++i) {
        const float nodeDuration = VFXNodeIsPassthrough(asset.nodes[i].type)
            ? 0.0f : asset.nodes[i].duration;
        outDuration = (std::max)(outDuration, outStartTimes[i] + nodeDuration);
    }
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
    // カタログ用メタデータ。空でも書き出す (キーが常に在ることで、AI が
    // 「未設定」と「この版では存在しない」を区別できる)。
    root.insert("description", asset.description);
    toml::array tags;
    for (const auto& tag : asset.tags) tags.push_back(tag);
    root.insert("tags", std::move(tags));
    toml::array requiredRoles;
    for (const auto& role : asset.requiredRoles) requiredRoles.push_back(role);
    root.insert("requiredRoles", std::move(requiredRoles));
    root.insert("thumbnailTime", asset.thumbnailTime);
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
        table.insert("eventName", link.eventName);
        links.push_back(std::move(table));
    }
    root.insert("links", std::move(links));
    toml::array dependencies;
    for (const auto& dependency : CollectVFXGraphDependencies(asset))
        dependencies.push_back(dependency);
    root.insert("dependencies", std::move(dependencies));
    toml::array groups;
    for (const auto& group : asset.groups) {
        toml::table table;
        table.insert("id", static_cast<std::int64_t>(group.id));
        table.insert("title", group.title);
        table.insert("note", group.note);
        table.insert("x", group.x);
        table.insert("y", group.y);
        table.insert("width", group.width);
        table.insert("height", group.height);
        table.insert("color", WriteVector4(group.color));
        // 由来を持つ枠だけがこの 3 つを持つ。手作りの枠では空/0 のまま。
        table.insert("sourceTemplate", group.sourceTemplate);
        table.insert("sourceTemplateVersion",
                     static_cast<std::int64_t>(group.sourceTemplateVersion));
        toml::array memberNodes;
        for (const int nodeId : group.memberNodes)
            memberNodes.push_back(static_cast<std::int64_t>(nodeId));
        table.insert("memberNodes", std::move(memberNodes));
        groups.push_back(std::move(table));
    }
    root.insert("groups", std::move(groups));
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
    loaded.version = (std::max)(loaded.version, 4);
    loaded.name = result["name"].value_or(std::string{ "VFX Graph" });
    loaded.description = result["description"].value_or(std::string{});
    if (const auto* tags = result["tags"].as_array()) {
        for (const auto& tag : *tags)
            if (const auto value = tag.value<std::string>(); value && !value->empty())
                loaded.tags.push_back(*value);
    }
    if (const auto* roles = result["requiredRoles"].as_array()) {
        for (const auto& role : *roles)
            if (const auto value = role.value<std::string>(); value && !value->empty())
                loaded.requiredRoles.push_back(*value);
    }
    loaded.thumbnailTime = static_cast<float>(result["thumbnailTime"].value_or(-1.0));
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
                static_cast<float>((*table)["delay"].value_or(0.0)),
                (*table)["eventName"].value_or(std::string{})
            });
        }
    }
    if (const auto* dependencies = result["dependencies"].as_array()) {
        for (const auto& dependency : *dependencies)
            if (const auto value = dependency.value<std::string>())
                loaded.dependencies.push_back(*value);
    }
    if (const auto* groups = result["groups"].as_array()) {
        for (const auto& element : *groups) {
            const auto* table = element.as_table();
            if (table == nullptr) continue;
            VFXGraphGroup group;
            group.id = static_cast<int>((*table)["id"].value_or(std::int64_t{0}));
            group.title = (*table)["title"].value_or(std::string{ "Group" });
            group.note = (*table)["note"].value_or(std::string{});
            group.x = static_cast<float>((*table)["x"].value_or(0.0));
            group.y = static_cast<float>((*table)["y"].value_or(0.0));
            group.width = static_cast<float>((*table)["width"].value_or(320.0));
            group.height = static_cast<float>((*table)["height"].value_or(200.0));
            group.color = ReadVector4((*table)["color"], group.color);
            group.sourceTemplate = (*table)["sourceTemplate"].value_or(std::string{});
            group.sourceTemplateVersion =
                static_cast<int>((*table)["sourceTemplateVersion"].value_or(std::int64_t{0}));
            if (const auto* members = (*table)["memberNodes"].as_array()) {
                for (const auto& member : *members)
                    if (const auto value = member.value<std::int64_t>())
                        group.memberNodes.push_back(static_cast<int>(*value));
            }
            loaded.groups.push_back(std::move(group));
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
