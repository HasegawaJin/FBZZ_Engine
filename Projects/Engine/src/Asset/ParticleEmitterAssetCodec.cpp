// FBZZ Engine
// ParticleEmitterAssetCodec.cpp | fbzz::asset
// ParticleEmitterの全authoringモジュールを欠落なくTOMLへ保存・復元する
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cstdint>
#include <string>

namespace fbzz::asset {
namespace {

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

float ReadFloat(const toml::table& table, const char* key, float fallback)
{
    return static_cast<float>(table[key].value_or(static_cast<double>(fallback)));
}

int ReadInt(const toml::table& table, const char* key, int fallback)
{
    return static_cast<int>(table[key].value_or(static_cast<std::int64_t>(fallback)));
}

template<class T>
T ReadEnum(const toml::table& table, const char* key, T fallback, int maximum)
{
    return static_cast<T>(std::clamp(ReadInt(table, key, static_cast<int>(fallback)), 0, maximum));
}

void WriteCurve(toml::table& table, const char* name, const scene::ParticleCurve& curve)
{
    toml::array array;
    const std::uint32_t count = (std::min)(curve.keyCount,
        static_cast<std::uint32_t>(curve.keys.size()));
    for (std::uint32_t index = 0; index < count; ++index)
        array.push_back(toml::array{ curve.keys[index].time, curve.keys[index].value });
    table.insert(name, std::move(array));
}

void ReadCurve(const toml::table& table, const char* name, scene::ParticleCurve& curve)
{
    const auto* array = table[name].as_array();
    if (array == nullptr || array->empty()) return;
    curve.keyCount = static_cast<std::uint32_t>((std::min)(array->size(), curve.keys.size()));
    for (std::uint32_t index = 0; index < curve.keyCount; ++index) {
        const auto* key = (*array)[index].as_array();
        if (key == nullptr || key->size() < 2) continue;
        curve.keys[index].time = static_cast<float>((*key)[0].value_or(0.0));
        curve.keys[index].value = static_cast<float>((*key)[1].value_or(0.0));
    }
}

} // namespace

toml::table SerializeParticleEmitterSettings(const scene::ParticleEmitter& emitter)
{
    toml::table table;
#define FBZZ_VFX_FLOAT(name) table.insert(#name, emitter.name)
#define FBZZ_VFX_INT(name) table.insert(#name, static_cast<std::int64_t>(emitter.name))
#define FBZZ_VFX_BOOL(name) table.insert(#name, emitter.name)
#define FBZZ_VFX_STRING(name) table.insert(#name, emitter.name)
    table.insert("emitPosition", WriteVector3(emitter.emitPosition));
    table.insert("emitVelocity", WriteVector3(emitter.emitVelocity));
    table.insert("colorStart", WriteVector4(emitter.colorStart));
    table.insert("colorEnd", WriteVector4(emitter.colorEnd));
    table.insert("gravity", WriteVector3(emitter.gravity));
    table.insert("boxExtents", WriteVector3(emitter.boxExtents));
    FBZZ_VFX_FLOAT(velocitySpread); FBZZ_VFX_FLOAT(sizeStart); FBZZ_VFX_FLOAT(sizeEnd);
    FBZZ_VFX_FLOAT(lifetime); FBZZ_VFX_FLOAT(lifetimeRandom); FBZZ_VFX_FLOAT(emitRate);
    FBZZ_VFX_INT(maxParticles); FBZZ_VFX_INT(randomSeed); FBZZ_VFX_BOOL(enabled);
    FBZZ_VFX_BOOL(playing); FBZZ_VFX_BOOL(loop); FBZZ_VFX_FLOAT(duration);
    FBZZ_VFX_FLOAT(startDelay); FBZZ_VFX_BOOL(clearOnStop); FBZZ_VFX_INT(shape);
    FBZZ_VFX_FLOAT(sphereRadius); FBZZ_VFX_FLOAT(coneAngleDegrees); FBZZ_VFX_FLOAT(coneRadius);
    FBZZ_VFX_STRING(meshShapePath); FBZZ_VFX_INT(meshShapeIndex); FBZZ_VFX_FLOAT(meshShapeScale);
    FBZZ_VFX_BOOL(meshShapeFollowSkinnedAnimation); FBZZ_VFX_INT(blendMode); FBZZ_VFX_INT(sortMode);
    FBZZ_VFX_INT(simulationMode); FBZZ_VFX_INT(simulationSpace); FBZZ_VFX_INT(renderMode);
    FBZZ_VFX_FLOAT(stretchedVelocityScale); FBZZ_VFX_FLOAT(stretchedLengthScale);
    FBZZ_VFX_INT(collisionMode); FBZZ_VFX_INT(collisionResponse); FBZZ_VFX_FLOAT(collisionRadius);
    FBZZ_VFX_FLOAT(collisionBounciness); FBZZ_VFX_FLOAT(collisionDamping); FBZZ_VFX_FLOAT(collisionPlaneY);
    FBZZ_VFX_STRING(materialPath); FBZZ_VFX_STRING(meshParticlePath); FBZZ_VFX_STRING(texturePath); FBZZ_VFX_INT(spriteColumns);
    FBZZ_VFX_INT(spriteRows); FBZZ_VFX_INT(spriteStartFrame); FBZZ_VFX_INT(spriteEndFrame);
    FBZZ_VFX_INT(flipbookMode); FBZZ_VFX_FLOAT(flipbookFramesPerSecond); FBZZ_VFX_BOOL(flipbookFrameBlending);
    FBZZ_VFX_BOOL(motionVectorFlipbook); FBZZ_VFX_STRING(motionVectorTexturePath); FBZZ_VFX_FLOAT(motionVectorStrength);
    FBZZ_VFX_FLOAT(sizeCurvePower); FBZZ_VFX_FLOAT(colorCurvePower); FBZZ_VFX_FLOAT(velocityDamping);
    FBZZ_VFX_FLOAT(angularVelocityMin); FBZZ_VFX_FLOAT(angularVelocityMax);
    FBZZ_VFX_BOOL(useSizeCurve); FBZZ_VFX_BOOL(useVelocityCurve); FBZZ_VFX_BOOL(useColorGradient);
    FBZZ_VFX_FLOAT(rateOverDistance); FBZZ_VFX_BOOL(prewarm); FBZZ_VFX_STRING(birthSubEmitter);
    FBZZ_VFX_STRING(deathSubEmitter); FBZZ_VFX_STRING(collisionSubEmitter); FBZZ_VFX_INT(subEmitterBurstCount);
    FBZZ_VFX_BOOL(softParticles); FBZZ_VFX_FLOAT(softParticleFadeDistance);
    FBZZ_VFX_BOOL(distortion); FBZZ_VFX_FLOAT(distortionStrength); FBZZ_VFX_BOOL(sixWayLighting);
    FBZZ_VFX_FLOAT(lightingStrength); FBZZ_VFX_FLOAT(emissiveScale);
    FBZZ_VFX_BOOL(cullingEnabled); FBZZ_VFX_FLOAT(cullingBoundsPadding); FBZZ_VFX_BOOL(lodEnabled);
    FBZZ_VFX_FLOAT(lodNearDistance); FBZZ_VFX_FLOAT(lodFarDistance); FBZZ_VFX_FLOAT(lodNearRateScale);
    FBZZ_VFX_FLOAT(lodFarRateScale); FBZZ_VFX_FLOAT(screenCoverageThreshold); FBZZ_VFX_BOOL(pauseWhenCulled);
    FBZZ_VFX_FLOAT(noiseStrength); FBZZ_VFX_FLOAT(noiseFrequency); FBZZ_VFX_FLOAT(noiseSpeed);
    FBZZ_VFX_BOOL(receiveForceFields);
#undef FBZZ_VFX_FLOAT
#undef FBZZ_VFX_INT
#undef FBZZ_VFX_BOOL
#undef FBZZ_VFX_STRING

    WriteCurve(table, "sizeCurve", emitter.sizeCurve);
    WriteCurve(table, "velocityCurve", emitter.velocityCurve);
    toml::array gradient;
    const std::uint32_t gradientCount = (std::min)(emitter.colorGradient.keyCount,
        static_cast<std::uint32_t>(emitter.colorGradient.keys.size()));
    for (std::uint32_t index = 0; index < gradientCount; ++index) {
        const auto& key = emitter.colorGradient.keys[index];
        gradient.push_back(toml::array{ key.time, key.color.x, key.color.y,
                                        key.color.z, key.color.w });
    }
    table.insert("colorGradient", std::move(gradient));
    toml::array bursts;
    for (const auto& burst : emitter.bursts) {
        toml::table item;
        item.insert("time", burst.time);
        item.insert("count", static_cast<std::int64_t>(burst.count));
        item.insert("cycles", static_cast<std::int64_t>(burst.cycles));
        item.insert("interval", burst.interval);
        item.insert("probability", burst.probability);
        bursts.push_back(std::move(item));
    }
    table.insert("bursts", std::move(bursts));
    return table;
}

void DeserializeParticleEmitterSettings(const toml::table& table,
                                        scene::ParticleEmitter& emitter)
{
#define FBZZ_VFX_FLOAT(name) emitter.name = ReadFloat(table, #name, emitter.name)
#define FBZZ_VFX_INT(name) emitter.name = ReadInt(table, #name, emitter.name)
#define FBZZ_VFX_BOOL(name) emitter.name = table[#name].value_or(emitter.name)
#define FBZZ_VFX_STRING(name) emitter.name = table[#name].value_or(emitter.name)
    emitter.emitPosition = ReadVector3(table["emitPosition"], emitter.emitPosition);
    emitter.emitVelocity = ReadVector3(table["emitVelocity"], emitter.emitVelocity);
    // version 1の.vfxが使っていた短縮名も読めるようにして既存アセットを移行不要にする。
    if (!table.contains("emitVelocity"))
        emitter.emitVelocity = ReadVector3(table["velocity"], emitter.emitVelocity);
    emitter.colorStart = ReadVector4(table["colorStart"], emitter.colorStart);
    emitter.colorEnd = ReadVector4(table["colorEnd"], emitter.colorEnd);
    emitter.gravity = ReadVector3(table["gravity"], emitter.gravity);
    emitter.boxExtents = ReadVector3(table["boxExtents"], emitter.boxExtents);
    FBZZ_VFX_FLOAT(velocitySpread); FBZZ_VFX_FLOAT(sizeStart); FBZZ_VFX_FLOAT(sizeEnd);
    FBZZ_VFX_FLOAT(lifetime); FBZZ_VFX_FLOAT(lifetimeRandom); FBZZ_VFX_FLOAT(emitRate);
    FBZZ_VFX_INT(maxParticles);
    emitter.randomSeed = static_cast<std::uint32_t>((std::max)(ReadInt(table, "randomSeed", 1), 1));
    FBZZ_VFX_BOOL(enabled); FBZZ_VFX_BOOL(playing); FBZZ_VFX_BOOL(loop); FBZZ_VFX_FLOAT(duration);
    FBZZ_VFX_FLOAT(startDelay); FBZZ_VFX_BOOL(clearOnStop);
    emitter.shape = ReadEnum(table, "shape", emitter.shape, 4);
    FBZZ_VFX_FLOAT(sphereRadius); FBZZ_VFX_FLOAT(coneAngleDegrees); FBZZ_VFX_FLOAT(coneRadius);
    FBZZ_VFX_STRING(meshShapePath); FBZZ_VFX_INT(meshShapeIndex); FBZZ_VFX_FLOAT(meshShapeScale);
    FBZZ_VFX_BOOL(meshShapeFollowSkinnedAnimation);
    emitter.blendMode = ReadEnum(table, "blendMode", emitter.blendMode, 1);
    emitter.sortMode = ReadEnum(table, "sortMode", emitter.sortMode, 1);
    emitter.simulationMode = ReadEnum(table, "simulationMode", emitter.simulationMode, 1);
    if (!table.contains("simulationMode") && table["gpu"].value_or(false))
        emitter.simulationMode = scene::ParticleSimulationMode::Gpu;
    emitter.simulationSpace = ReadEnum(table, "simulationSpace", emitter.simulationSpace, 1);
    emitter.renderMode = ReadEnum(table, "renderMode", emitter.renderMode, 3);
    FBZZ_VFX_FLOAT(stretchedVelocityScale); FBZZ_VFX_FLOAT(stretchedLengthScale);
    emitter.collisionMode = ReadEnum(table, "collisionMode", emitter.collisionMode, 3);
    emitter.collisionResponse = ReadEnum(table, "collisionResponse", emitter.collisionResponse, 2);
    FBZZ_VFX_FLOAT(collisionRadius); FBZZ_VFX_FLOAT(collisionBounciness);
    FBZZ_VFX_FLOAT(collisionDamping); FBZZ_VFX_FLOAT(collisionPlaneY);
    FBZZ_VFX_STRING(materialPath); FBZZ_VFX_STRING(meshParticlePath); FBZZ_VFX_STRING(texturePath); FBZZ_VFX_INT(spriteColumns);
    FBZZ_VFX_INT(spriteRows); FBZZ_VFX_INT(spriteStartFrame); FBZZ_VFX_INT(spriteEndFrame);
    emitter.flipbookMode = ReadEnum(table, "flipbookMode", emitter.flipbookMode, 3);
    FBZZ_VFX_FLOAT(flipbookFramesPerSecond); FBZZ_VFX_BOOL(flipbookFrameBlending);
    FBZZ_VFX_BOOL(motionVectorFlipbook); FBZZ_VFX_STRING(motionVectorTexturePath); FBZZ_VFX_FLOAT(motionVectorStrength);
    FBZZ_VFX_FLOAT(sizeCurvePower); FBZZ_VFX_FLOAT(colorCurvePower); FBZZ_VFX_FLOAT(velocityDamping);
    FBZZ_VFX_FLOAT(angularVelocityMin); FBZZ_VFX_FLOAT(angularVelocityMax);
    FBZZ_VFX_BOOL(useSizeCurve); FBZZ_VFX_BOOL(useVelocityCurve); FBZZ_VFX_BOOL(useColorGradient);
    FBZZ_VFX_FLOAT(rateOverDistance); FBZZ_VFX_BOOL(prewarm); FBZZ_VFX_STRING(birthSubEmitter);
    FBZZ_VFX_STRING(deathSubEmitter); FBZZ_VFX_STRING(collisionSubEmitter); FBZZ_VFX_INT(subEmitterBurstCount);
    FBZZ_VFX_BOOL(softParticles); FBZZ_VFX_FLOAT(softParticleFadeDistance);
    FBZZ_VFX_BOOL(distortion); FBZZ_VFX_FLOAT(distortionStrength); FBZZ_VFX_BOOL(sixWayLighting);
    FBZZ_VFX_FLOAT(lightingStrength); FBZZ_VFX_FLOAT(emissiveScale);
    FBZZ_VFX_BOOL(cullingEnabled); FBZZ_VFX_FLOAT(cullingBoundsPadding); FBZZ_VFX_BOOL(lodEnabled);
    FBZZ_VFX_FLOAT(lodNearDistance); FBZZ_VFX_FLOAT(lodFarDistance); FBZZ_VFX_FLOAT(lodNearRateScale);
    FBZZ_VFX_FLOAT(lodFarRateScale); FBZZ_VFX_FLOAT(screenCoverageThreshold); FBZZ_VFX_BOOL(pauseWhenCulled);
    FBZZ_VFX_FLOAT(noiseStrength); FBZZ_VFX_FLOAT(noiseFrequency); FBZZ_VFX_FLOAT(noiseSpeed);
    FBZZ_VFX_BOOL(receiveForceFields);
#undef FBZZ_VFX_FLOAT
#undef FBZZ_VFX_INT
#undef FBZZ_VFX_BOOL
#undef FBZZ_VFX_STRING

    ReadCurve(table, "sizeCurve", emitter.sizeCurve);
    ReadCurve(table, "velocityCurve", emitter.velocityCurve);
    if (const auto* gradient = table["colorGradient"].as_array(); gradient != nullptr && !gradient->empty()) {
        emitter.colorGradient.keyCount = static_cast<std::uint32_t>(
            (std::min)(gradient->size(), emitter.colorGradient.keys.size()));
        for (std::uint32_t index = 0; index < emitter.colorGradient.keyCount; ++index) {
            const auto* key = (*gradient)[index].as_array();
            if (key == nullptr || key->size() < 5) continue;
            emitter.colorGradient.keys[index].time = static_cast<float>((*key)[0].value_or(0.0));
            emitter.colorGradient.keys[index].color = {
                static_cast<float>((*key)[1].value_or(1.0)),
                static_cast<float>((*key)[2].value_or(1.0)),
                static_cast<float>((*key)[3].value_or(1.0)),
                static_cast<float>((*key)[4].value_or(1.0))
            };
        }
    }
    emitter.bursts.clear();
    if (const auto* bursts = table["bursts"].as_array()) {
        for (const auto& node : *bursts) {
            const auto* item = node.as_table();
            if (item == nullptr) continue;
            scene::ParticleBurst burst;
            burst.time = ReadFloat(*item, "time", burst.time);
            burst.count = ReadInt(*item, "count", burst.count);
            burst.cycles = (std::max)(ReadInt(*item, "cycles", burst.cycles), 1);
            burst.interval = (std::max)(ReadFloat(*item, "interval", burst.interval), 0.0f);
            burst.probability = std::clamp(ReadFloat(*item, "probability", burst.probability), 0.0f, 1.0f);
            emitter.bursts.push_back(burst);
        }
    }
    if (emitter.bursts.empty()) {
        const int legacyBurstCount = ReadInt(table, "burstCount", 0);
        if (legacyBurstCount > 0) emitter.bursts.push_back({ .time = 0.0f, .count = legacyBurstCount });
    }
    // アセットから復元した時点ではGPU/CPU双方のランタイム状態を必ず初期状態へ戻す。
    emitter.randomState = emitter.randomSeed;
    emitter.ResetPlayback();
}

} // namespace fbzz::asset
