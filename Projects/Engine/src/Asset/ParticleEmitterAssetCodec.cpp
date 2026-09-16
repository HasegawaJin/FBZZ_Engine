/// @file    ParticleEmitterAssetCodec.cpp
/// @brief   ParticleEmitterの全authoringモジュールを欠落なくTOMLへ保存・復元する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>

#include <Engine/Core/Logger.hpp>
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
    table.insert(name, SerializeParticleCurve(curve));
}

void ReadCurve(const toml::table& table, const char* name, scene::ParticleCurve& curve)
{
    DeserializeParticleCurve(table, name, curve);
}

toml::table WriteForce(const scene::ForceFieldSettings& force)
{
    toml::table item;
    item.insert("enabled", force.enabled);
    item.insert("fieldType", static_cast<std::int64_t>(force.fieldType));
    item.insert("space", static_cast<std::int64_t>(force.space));
    item.insert("strength", force.strength);
    item.insert("radius", force.radius);
    item.insert("falloffPower", force.falloffPower);
    item.insert("direction", WriteVector3(force.direction));
    item.insert("noiseFrequency", force.noiseFrequency);
    item.insert("noiseSpeed", force.noiseSpeed);
    item.insert("channels", static_cast<std::int64_t>(force.channels));
    item.insert("vectorFieldPath", force.vectorFieldPath);
    item.insert("vectorFieldExtents", WriteVector3(force.vectorFieldExtents));
    item.insert("vectorFieldTightness", force.vectorFieldTightness);
    return item;
}

scene::ForceFieldSettings ReadForce(const toml::table& item)
{
    scene::ForceFieldSettings force;
    force.enabled = item["enabled"].value_or(force.enabled);
    force.fieldType = ReadEnum(item, "fieldType", force.fieldType,
                               scene::kForceFieldTypeCount - 1);
    force.space = ReadEnum(item, "space", force.space, 1);
    force.strength = ReadFloat(item, "strength", force.strength);
    force.radius = ReadFloat(item, "radius", force.radius);
    force.falloffPower = ReadFloat(item, "falloffPower", force.falloffPower);
    force.direction = ReadVector3(item["direction"], force.direction);
    force.noiseFrequency = ReadFloat(item, "noiseFrequency", force.noiseFrequency);
    force.noiseSpeed = ReadFloat(item, "noiseSpeed", force.noiseSpeed);
    force.channels = static_cast<std::uint32_t>(
        item["channels"].value_or(static_cast<std::int64_t>(force.channels)));
    force.vectorFieldPath = item["vectorFieldPath"].value_or(force.vectorFieldPath);
    force.vectorFieldExtents = ReadVector3(item["vectorFieldExtents"], force.vectorFieldExtents);
    force.vectorFieldTightness = ReadFloat(item, "vectorFieldTightness", force.vectorFieldTightness);
    return force;
}

// 内蔵の力が個別フィールドだった頃 (〜2026-09-11) の .scene / .particle / .vfx を読む。
//
// WHY 自動で移すか: 見た目のキー (.mat へ移した 34 項目) は «一度だけ警告して捨てる» で
//     済んだ。あれは «素材側で設定し直す» という行き先が人間に見える話だったからだ。
//     こちらは重力そのものなので、捨てると既存のエフェクトが全部その場に浮く。
//     しかも «浮いている» は設定ミスと区別が付かない。値は移して、形だけ変える。
void MigrateLegacyForces(const toml::table& table, scene::ParticleEmitterSettings& emitter)
{
    using scene::ForceFieldSettings;
    using scene::ForceFieldSpace;
    using scene::ForceFieldType;

    // 既定の localForces (重力 1 本) は «新規エミッターの初期値» であって、
    // このファイルが意図した内容ではない。旧ファイルの記述だけを正とする。
    emitter.localForces.clear();

    const math::Vector3 gravity = ReadVector3(table["gravity"], math::Vector3{ 0.0f, -5.0f, 0.0f });
    const float gravityMagnitude = gravity.Length();
    if (gravityMagnitude > 1.0e-6f) {
        ForceFieldSettings wind;
        wind.fieldType = ForceFieldType::Wind;
        wind.space     = ForceFieldSpace::World;
        wind.direction = gravity * (1.0f / gravityMagnitude);
        wind.strength  = gravityMagnitude;
        wind.radius    = 0.0f;
        emitter.localForces.push_back(wind);
    }

    if (const float damping = ReadFloat(table, "velocityDamping", 0.0f); damping > 0.0f) {
        ForceFieldSettings drag;
        drag.fieldType = ForceFieldType::Drag;
        drag.space     = ForceFieldSpace::World;
        drag.strength  = damping;
        drag.radius    = 0.0f;
        emitter.localForces.push_back(drag);
    }

    if (const float noise = ReadFloat(table, "noiseStrength", 0.0f); noise > 0.0f) {
        ForceFieldSettings turbulence;
        turbulence.fieldType      = ForceFieldType::Turbulence;
        turbulence.space          = ForceFieldSpace::World;
        turbulence.strength       = noise;
        turbulence.radius         = 0.0f;
        turbulence.noiseFrequency = ReadFloat(table, "noiseFrequency", 0.5f);
        turbulence.noiseSpeed     = ReadFloat(table, "noiseSpeed", 1.0f);
        emitter.localForces.push_back(turbulence);
    }

    if (const float orbital = ReadFloat(table, "orbitalVelocity", 0.0f); orbital != 0.0f) {
        ForceFieldSettings vortex;
        vortex.fieldType = ForceFieldType::Vortex;
        vortex.space     = ForceFieldSpace::Emitter;
        vortex.direction = ReadVector3(table["orbitalAxis"], math::Vector3{ 0.0f, 1.0f, 0.0f });
        vortex.strength  = orbital;
        vortex.radius    = 0.0f;
        emitter.localForces.push_back(vortex);
    }

    // 旧 radialVelocity は «正で外向き / 負で吸い込み»。Repulse の strength と同じ符号規約
    // なので、負のまま Repulse として持たせれば挙動が一致する (Attract へ倒す必要はない)。
    if (const float radial = ReadFloat(table, "radialVelocity", 0.0f); radial != 0.0f) {
        ForceFieldSettings repulse;
        repulse.fieldType = ForceFieldType::Repulse;
        repulse.space     = ForceFieldSpace::Emitter;
        repulse.strength  = radial;
        repulse.radius    = 0.0f;
        emitter.localForces.push_back(repulse);
    }
}

} // namespace

// カーブとグラデーションは補間モードとキー配列を 1 テーブルへまとめて保存する。
namespace {

toml::array WriteCurveKeys(const scene::ParticleCurve& curve)
{
    toml::array array;
    const std::uint32_t count = (std::min)(curve.keyCount,
        static_cast<std::uint32_t>(curve.keys.size()));
    for (std::uint32_t index = 0; index < count; ++index)
        array.push_back(toml::array{ curve.keys[index].time, curve.keys[index].value });
    return array;
}

// 補間モードを int から復元する。範囲外は Linear へ倒す (壊れたアセットで落とさない)。
scene::ParticleCurveInterpolation ReadInterpolation(const toml::node_view<const toml::node>& value)
{
    const int mode = static_cast<int>(value.value_or(std::int64_t{0}));
    return static_cast<scene::ParticleCurveInterpolation>(
        std::clamp(mode, 0, static_cast<int>(scene::ParticleCurveInterpolation::Smooth)));
}

// キー配列を取り出す。2 つの表記を両方受け付ける。
//   テーブル形式: curve = { interp = 0, keys = [[t, v], ...] }   ← Editor が書き出す形
//   配列形式:     curve = [[t, v], ...]                          ← .scene と手書きアセットの形
// WHY: 書き出しはテーブル形式だが、読み込みがテーブル形式しか受け付けていなかった。
//      手書きの .vfx (Assets/VFX/Templates/*) は .scene と同じ配列形式で書かれており、
//      colorGradient も dragCurve も丸ごと無視されて既定値のままロードされていた。
//      既定のグラデーションは「白 → 透明」なので、テンプレートの炎も煙も魔法も
//      すべて白い粒子として描かれる。テンプレートの色が出ない主因がこれ。
//      配列形式を受け付ければ既存アセットは一切書き換えずに直る。
const toml::array* CurveKeyArray(const toml::node_view<const toml::node>& value)
{
    if (const auto* table = value.as_table()) return (*table)["keys"].as_array();
    return value.as_array();
}

} // namespace

toml::table SerializeParticleCurve(const scene::ParticleCurve& curve)
{
    toml::table table;
    table.insert("interp", static_cast<std::int64_t>(curve.interpolation));
    table.insert("keys", WriteCurveKeys(curve));
    return table;
}

void DeserializeParticleCurve(const toml::table& table, const char* key,
                              scene::ParticleCurve& outCurve)
{
    const auto value = table[key];
    const auto* array = CurveKeyArray(value);
    if (array == nullptr || array->empty()) return;
    if (const auto* asTable = value.as_table())
        outCurve.interpolation = ReadInterpolation((*asTable)["interp"]);
    outCurve.keyCount = static_cast<std::uint32_t>((std::min)(array->size(), outCurve.keys.size()));
    for (std::uint32_t index = 0; index < outCurve.keyCount; ++index) {
        const auto* entry = (*array)[index].as_array();
        if (entry == nullptr || entry->size() < 2) continue;
        outCurve.keys[index].time = static_cast<float>((*entry)[0].value_or(0.0));
        outCurve.keys[index].value = static_cast<float>((*entry)[1].value_or(0.0));
    }
}

toml::table SerializeParticleGradient(const scene::ParticleGradient& gradient)
{
    toml::array array;
    const std::uint32_t count = (std::min)(gradient.keyCount,
        static_cast<std::uint32_t>(gradient.keys.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& key = gradient.keys[index];
        array.push_back(toml::array{ key.time, key.color.x, key.color.y, key.color.z, key.color.w });
    }
    toml::table table;
    table.insert("interp", static_cast<std::int64_t>(gradient.interpolation));
    table.insert("space", static_cast<std::int64_t>(gradient.colorSpace));
    table.insert("keys", std::move(array));
    return table;
}

void DeserializeParticleGradient(const toml::table& table, const char* key,
                                 scene::ParticleGradient& outGradient)
{
    const auto value = table[key];
    const auto* array = CurveKeyArray(value);
    if (array == nullptr || array->empty()) return;
    if (const auto* asTable = value.as_table()) {
        outGradient.interpolation = ReadInterpolation((*asTable)["interp"]);
        // 未記載の既存アセットは Gamma (従来の挙動) のまま読む。
        const int space = static_cast<int>((*asTable)["space"].value_or<std::int64_t>(
            static_cast<std::int64_t>(scene::ParticleColorSpace::Gamma)));
        outGradient.colorSpace = static_cast<scene::ParticleColorSpace>(
            std::clamp(space, 0, static_cast<int>(scene::ParticleColorSpace::Oklab)));
    }
    outGradient.keyCount =
        static_cast<std::uint32_t>((std::min)(array->size(), outGradient.keys.size()));
    for (std::uint32_t index = 0; index < outGradient.keyCount; ++index) {
        const auto* entry = (*array)[index].as_array();
        if (entry == nullptr || entry->size() < 5) continue;
        outGradient.keys[index].time = static_cast<float>((*entry)[0].value_or(0.0));
        outGradient.keys[index].color = {
            static_cast<float>((*entry)[1].value_or(1.0)),
            static_cast<float>((*entry)[2].value_or(1.0)),
            static_cast<float>((*entry)[3].value_or(1.0)),
            static_cast<float>((*entry)[4].value_or(1.0))
        };
    }
}

toml::table SerializeParticleEmitterSettings(const scene::ParticleEmitterSettings& emitter)
{
    toml::table table;
#define FBZZ_VFX_FLOAT(name) table.insert(#name, emitter.name)
#define FBZZ_VFX_INT(name) table.insert(#name, static_cast<std::int64_t>(emitter.name))
#define FBZZ_VFX_BOOL(name) table.insert(#name, emitter.name)
#define FBZZ_VFX_STRING(name) table.insert(#name, emitter.name)
#define FBZZ_VFX_FLOAT_IN(module, name) table.insert(#name, emitter.module.name)
#define FBZZ_VFX_INT_IN(module, name) table.insert(#name, static_cast<std::int64_t>(emitter.module.name))
#define FBZZ_VFX_BOOL_IN(module, name) table.insert(#name, emitter.module.name)
    table.insert("emitPosition", WriteVector3(emitter.emitPosition));
    table.insert("emitVelocity", WriteVector3(emitter.emitVelocity));
    table.insert("colorStart", WriteVector4(emitter.colorStart));
    table.insert("colorEnd", WriteVector4(emitter.colorEnd));
    table.insert("boxExtents", WriteVector3(emitter.boxExtents));
    table.insert("sizeAxisScale", WriteVector3(emitter.sizeAxisScale));
    FBZZ_VFX_FLOAT(velocitySpread); FBZZ_VFX_FLOAT(sizeStart); FBZZ_VFX_FLOAT(sizeEnd);
    FBZZ_VFX_FLOAT(lifetime); FBZZ_VFX_FLOAT(lifetimeRandom); FBZZ_VFX_FLOAT(emitRate);
    FBZZ_VFX_INT(maxParticles); FBZZ_VFX_INT(randomSeed); FBZZ_VFX_BOOL(enabled);
    FBZZ_VFX_BOOL(playing); FBZZ_VFX_BOOL(loop); FBZZ_VFX_FLOAT(duration);
    FBZZ_VFX_FLOAT(startDelay); FBZZ_VFX_BOOL(clearOnStop); FBZZ_VFX_INT(shape);
    FBZZ_VFX_FLOAT(sphereRadius); FBZZ_VFX_FLOAT(coneAngleDegrees); FBZZ_VFX_FLOAT(coneRadius);
    FBZZ_VFX_STRING(meshShapePath); FBZZ_VFX_INT(meshShapeIndex); FBZZ_VFX_FLOAT(meshShapeScale);
    FBZZ_VFX_BOOL(meshShapeFollowSkinnedAnimation); FBZZ_VFX_FLOAT(meshShapeNormalVelocity);
    FBZZ_VFX_INT(sortMode);
    FBZZ_VFX_INT(simulationMode); FBZZ_VFX_INT(simulationSpace); FBZZ_VFX_INT(renderMode);
    FBZZ_VFX_FLOAT(stretchedVelocityScale); FBZZ_VFX_FLOAT(stretchedLengthScale);
    FBZZ_VFX_INT(renderPriority);
    FBZZ_VFX_INT(collisionMode); FBZZ_VFX_INT(collisionResponse); FBZZ_VFX_FLOAT(collisionRadius);
    FBZZ_VFX_FLOAT(collisionBounciness); FBZZ_VFX_FLOAT(collisionDamping); FBZZ_VFX_FLOAT(collisionPlaneY);
    FBZZ_VFX_STRING(materialPath); FBZZ_VFX_STRING(meshParticlePath);
    FBZZ_VFX_FLOAT(colorVariation);
    FBZZ_VFX_FLOAT(sizeCurvePower); FBZZ_VFX_FLOAT(colorCurvePower);
    FBZZ_VFX_FLOAT(angularVelocityMin); FBZZ_VFX_FLOAT(angularVelocityMax);
    FBZZ_VFX_BOOL(useSizeCurve); FBZZ_VFX_BOOL(useVelocityCurve); FBZZ_VFX_BOOL(useColorGradient);
    FBZZ_VFX_BOOL(useEmitRateCurve); FBZZ_VFX_FLOAT(speedRange);
    FBZZ_VFX_BOOL(useSpeedSizeCurve); FBZZ_VFX_BOOL(useSpeedColorGradient);
    FBZZ_VFX_BOOL(useRotationCurve); FBZZ_VFX_BOOL(useDragCurve);
    FBZZ_VFX_FLOAT(inheritVelocity);
    FBZZ_VFX_FLOAT(rateOverDistance); FBZZ_VFX_BOOL(prewarm); FBZZ_VFX_STRING(birthSubEmitter);
    FBZZ_VFX_STRING(deathSubEmitter); FBZZ_VFX_STRING(collisionSubEmitter); FBZZ_VFX_INT(subEmitterBurstCount);
    FBZZ_VFX_FLOAT(subEmitterInheritVelocity);
    FBZZ_VFX_BOOL_IN(trail, trailEnabled); FBZZ_VFX_INT_IN(trail, trailPointCount);
    FBZZ_VFX_FLOAT_IN(trail, trailSampleInterval); FBZZ_VFX_FLOAT_IN(trail, trailWidthScale);
    FBZZ_VFX_FLOAT_IN(trail, trailAlphaScale);
    FBZZ_VFX_BOOL_IN(trail, trailRibbon); FBZZ_VFX_FLOAT_IN(trail, trailRibbonWidth);
    FBZZ_VFX_BOOL(blackbodyEnabled); FBZZ_VFX_FLOAT(blackbodyReferenceTemperature);
    FBZZ_VFX_FLOAT(blackbodyIntensity);
    FBZZ_VFX_BOOL_IN(culling, cullingEnabled); FBZZ_VFX_FLOAT_IN(culling, cullingBoundsPadding); FBZZ_VFX_BOOL_IN(culling, lodEnabled);
    FBZZ_VFX_FLOAT_IN(culling, lodNearDistance); FBZZ_VFX_FLOAT_IN(culling, lodFarDistance); FBZZ_VFX_FLOAT_IN(culling, lodNearRateScale);
    FBZZ_VFX_FLOAT_IN(culling, lodFarRateScale); FBZZ_VFX_FLOAT_IN(culling, screenCoverageThreshold); FBZZ_VFX_BOOL_IN(culling, pauseWhenCulled);
    FBZZ_VFX_BOOL_IN(light, lightEnabled); FBZZ_VFX_FLOAT_IN(light, lightRatio); FBZZ_VFX_INT_IN(light, lightMaxCount);
    FBZZ_VFX_FLOAT_IN(light, lightRange); FBZZ_VFX_BOOL_IN(light, lightRangeFromSize); FBZZ_VFX_FLOAT_IN(light, lightIntensity);
    FBZZ_VFX_BOOL_IN(light, lightUseParticleColor); FBZZ_VFX_BOOL_IN(light, lightFadeWithAlpha);
    FBZZ_VFX_BOOL(receiveForceFields);
#undef FBZZ_VFX_FLOAT
#undef FBZZ_VFX_INT
#undef FBZZ_VFX_BOOL
#undef FBZZ_VFX_STRING
#undef FBZZ_VFX_FLOAT_IN
#undef FBZZ_VFX_INT_IN
#undef FBZZ_VFX_BOOL_IN

    // FBZZ_VFX_INT は int を経由するため 0xFFFFFFFF が符号付きで潰れる。マスクは別に書く。
    table.insert("forceFieldChannels", static_cast<std::int64_t>(emitter.forceFieldChannels));
    table.insert("collisionLayerMask", static_cast<std::int64_t>(emitter.collisionLayerMask));
    table.insert("trailColorTint", WriteVector4(emitter.trail.trailColorTint));
    table.insert("lightColor", WriteVector4(emitter.light.lightColor));
    WriteCurve(table, "sizeCurve", emitter.sizeCurve);
    WriteCurve(table, "velocityCurve", emitter.velocityCurve);
    WriteCurve(table, "rotationCurve", emitter.rotationCurve);
    WriteCurve(table, "dragCurve", emitter.dragCurve);
    WriteCurve(table, "temperatureCurve", emitter.temperatureCurve);
    WriteCurve(table, "emitRateCurve", emitter.emitRateCurve);
    WriteCurve(table, "speedSizeCurve", emitter.speedSizeCurve);
    table.insert("colorGradient", SerializeParticleGradient(emitter.colorGradient));
    table.insert("speedColorGradient", SerializeParticleGradient(emitter.speedColorGradient));
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

    toml::array forces;
    for (const auto& force : emitter.localForces) forces.push_back(WriteForce(force));
    table.insert("localForces", std::move(forces));
    return table;
}

// 見た目の設定は .mat の [particle] へ移った (2026-08-24)。
// 旧いシーン / .vfx にはまだキーが残っているので、黙って捨てずに一度だけ知らせる。
//
// WHY 自動で .mat へ書き込まないか: 読み込みの副作用でアセットを書き換えると、
//     «開いただけでプロジェクトが変わる» ことになる。しかも 1 つの .mat を複数の
//     エミッターが共有していると、どのエミッターの値を採用すべきか決められない。
//     どこに何が残っているかだけ示して、移す判断は担当者に任せる。
void WarnLegacyParticleLookKeys(const toml::table& table)
{
    static constexpr const char* kLegacyKeys[] = {
        "blendMode", "alphaSource", "spriteColumns", "spriteRows", "spriteStartFrame",
        "spriteEndFrame", "flipbookMode", "flipbookFramesPerSecond", "flipbookFrameBlending",
        "spriteRandomStartFrame", "spriteRandomRow", "motionVectorFlipbook",
        "motionVectorTexturePath", "motionVectorStrength", "softParticles",
        "softParticleFadeDistance", "distortion", "distortionStrength", "distortionChromatic",
        "distortionTexturePath", "sixWayLighting", "lightingStrength", "smokeWrap",
        "smokeTransmission", "smokeBackScatterPower", "volumetric", "volumetricSteps",
        "volumetricDensity", "volumetricAnisotropy", "volumetricNoiseScale",
        "receiveShadows", "shadowStrength", "selfShadowStrength", "emissiveScale",
    };
    std::string found;
    for (const char* key : kLegacyKeys) {
        if (!table.contains(key)) continue;
        if (!found.empty()) found += ", ";
        found += key;
    }
    if (found.empty()) return;
    FBZZ_LOG_WARN("ParticleEmitter: これらの見た目設定は .mat の [particle] へ移りました。"
                  "値は読み込まれません — materialPath の .mat 側で設定し直してください: %s",
                  found.c_str());
}

void DeserializeParticleEmitterSettings(const toml::table& table,
                                        scene::ParticleEmitterSettings& emitter)
{
    WarnLegacyParticleLookKeys(table);
#define FBZZ_VFX_FLOAT(name) emitter.name = ReadFloat(table, #name, emitter.name)
#define FBZZ_VFX_INT(name) emitter.name = ReadInt(table, #name, emitter.name)
#define FBZZ_VFX_BOOL(name) emitter.name = table[#name].value_or(emitter.name)
#define FBZZ_VFX_STRING(name) emitter.name = table[#name].value_or(emitter.name)
#define FBZZ_VFX_FLOAT_IN(module, name) emitter.module.name = ReadFloat(table, #name, emitter.module.name)
#define FBZZ_VFX_INT_IN(module, name) emitter.module.name = ReadInt(table, #name, emitter.module.name)
#define FBZZ_VFX_BOOL_IN(module, name) emitter.module.name = table[#name].value_or(emitter.module.name)
    emitter.emitPosition = ReadVector3(table["emitPosition"], emitter.emitPosition);
    emitter.emitVelocity = ReadVector3(table["emitVelocity"], emitter.emitVelocity);
    emitter.colorStart = ReadVector4(table["colorStart"], emitter.colorStart);
    emitter.colorEnd = ReadVector4(table["colorEnd"], emitter.colorEnd);
    // 旧 gravity キーは MigrateLegacyForces が localForces へ組み直す (この関数の末尾)。
    emitter.boxExtents = ReadVector3(table["boxExtents"], emitter.boxExtents);
    emitter.sizeAxisScale = ReadVector3(table["sizeAxisScale"], emitter.sizeAxisScale);
    FBZZ_VFX_FLOAT(velocitySpread); FBZZ_VFX_FLOAT(sizeStart); FBZZ_VFX_FLOAT(sizeEnd);
    FBZZ_VFX_FLOAT(lifetime); FBZZ_VFX_FLOAT(lifetimeRandom); FBZZ_VFX_FLOAT(emitRate);
    FBZZ_VFX_INT(maxParticles);
    emitter.randomSeed = static_cast<std::uint32_t>((std::max)(ReadInt(table, "randomSeed", 1), 1));
    FBZZ_VFX_BOOL(enabled); FBZZ_VFX_BOOL(playing); FBZZ_VFX_BOOL(loop); FBZZ_VFX_FLOAT(duration);
    FBZZ_VFX_FLOAT(startDelay); FBZZ_VFX_BOOL(clearOnStop);
    emitter.shape = ReadEnum(table, "shape", emitter.shape, 4);
    FBZZ_VFX_FLOAT(sphereRadius); FBZZ_VFX_FLOAT(coneAngleDegrees); FBZZ_VFX_FLOAT(coneRadius);
    FBZZ_VFX_STRING(meshShapePath); FBZZ_VFX_INT(meshShapeIndex); FBZZ_VFX_FLOAT(meshShapeScale);
    FBZZ_VFX_BOOL(meshShapeFollowSkinnedAnimation); FBZZ_VFX_FLOAT(meshShapeNormalVelocity);
    emitter.sortMode = ReadEnum(table, "sortMode", emitter.sortMode, 1);
    emitter.simulationMode = ReadEnum(table, "simulationMode", emitter.simulationMode, 1);
    emitter.simulationSpace = ReadEnum(table, "simulationSpace", emitter.simulationSpace, 1);
    emitter.renderMode = ReadEnum(table, "renderMode", emitter.renderMode, 3);
    FBZZ_VFX_FLOAT(stretchedVelocityScale); FBZZ_VFX_FLOAT(stretchedLengthScale);
    FBZZ_VFX_INT(renderPriority);
    emitter.collisionMode = ReadEnum(table, "collisionMode", emitter.collisionMode, 3);
    emitter.collisionResponse = ReadEnum(table, "collisionResponse", emitter.collisionResponse, 2);
    FBZZ_VFX_FLOAT(collisionRadius); FBZZ_VFX_FLOAT(collisionBounciness);
    FBZZ_VFX_FLOAT(collisionDamping); FBZZ_VFX_FLOAT(collisionPlaneY);
    FBZZ_VFX_STRING(materialPath); FBZZ_VFX_STRING(meshParticlePath);
    FBZZ_VFX_FLOAT(colorVariation);
    FBZZ_VFX_FLOAT(sizeCurvePower); FBZZ_VFX_FLOAT(colorCurvePower);
    FBZZ_VFX_FLOAT(angularVelocityMin); FBZZ_VFX_FLOAT(angularVelocityMax);
    FBZZ_VFX_BOOL(useSizeCurve); FBZZ_VFX_BOOL(useVelocityCurve); FBZZ_VFX_BOOL(useColorGradient);
    FBZZ_VFX_BOOL(useEmitRateCurve); FBZZ_VFX_FLOAT(speedRange);
    FBZZ_VFX_BOOL(useSpeedSizeCurve); FBZZ_VFX_BOOL(useSpeedColorGradient);
    FBZZ_VFX_BOOL(useRotationCurve); FBZZ_VFX_BOOL(useDragCurve);
    FBZZ_VFX_FLOAT(inheritVelocity);
    FBZZ_VFX_FLOAT(rateOverDistance); FBZZ_VFX_BOOL(prewarm); FBZZ_VFX_STRING(birthSubEmitter);
    FBZZ_VFX_STRING(deathSubEmitter); FBZZ_VFX_STRING(collisionSubEmitter); FBZZ_VFX_INT(subEmitterBurstCount);
    FBZZ_VFX_FLOAT(subEmitterInheritVelocity);
    FBZZ_VFX_BOOL_IN(trail, trailEnabled); FBZZ_VFX_INT_IN(trail, trailPointCount);
    FBZZ_VFX_FLOAT_IN(trail, trailSampleInterval); FBZZ_VFX_FLOAT_IN(trail, trailWidthScale);
    FBZZ_VFX_FLOAT_IN(trail, trailAlphaScale);
    FBZZ_VFX_BOOL_IN(trail, trailRibbon); FBZZ_VFX_FLOAT_IN(trail, trailRibbonWidth);
    FBZZ_VFX_BOOL(blackbodyEnabled); FBZZ_VFX_FLOAT(blackbodyReferenceTemperature);
    FBZZ_VFX_FLOAT(blackbodyIntensity);
    FBZZ_VFX_BOOL_IN(culling, cullingEnabled); FBZZ_VFX_FLOAT_IN(culling, cullingBoundsPadding); FBZZ_VFX_BOOL_IN(culling, lodEnabled);
    FBZZ_VFX_FLOAT_IN(culling, lodNearDistance); FBZZ_VFX_FLOAT_IN(culling, lodFarDistance); FBZZ_VFX_FLOAT_IN(culling, lodNearRateScale);
    FBZZ_VFX_FLOAT_IN(culling, lodFarRateScale); FBZZ_VFX_FLOAT_IN(culling, screenCoverageThreshold); FBZZ_VFX_BOOL_IN(culling, pauseWhenCulled);
    FBZZ_VFX_BOOL_IN(light, lightEnabled); FBZZ_VFX_FLOAT_IN(light, lightRatio); FBZZ_VFX_INT_IN(light, lightMaxCount);
    FBZZ_VFX_FLOAT_IN(light, lightRange); FBZZ_VFX_BOOL_IN(light, lightRangeFromSize); FBZZ_VFX_FLOAT_IN(light, lightIntensity);
    FBZZ_VFX_BOOL_IN(light, lightUseParticleColor); FBZZ_VFX_BOOL_IN(light, lightFadeWithAlpha);
    FBZZ_VFX_BOOL(receiveForceFields);
#undef FBZZ_VFX_FLOAT
#undef FBZZ_VFX_INT
#undef FBZZ_VFX_BOOL
#undef FBZZ_VFX_STRING
#undef FBZZ_VFX_FLOAT_IN
#undef FBZZ_VFX_INT_IN
#undef FBZZ_VFX_BOOL_IN

    emitter.forceFieldChannels = static_cast<std::uint32_t>(
        table["forceFieldChannels"].value_or(
            static_cast<std::int64_t>(emitter.forceFieldChannels)));
    emitter.collisionLayerMask = static_cast<std::uint32_t>(
        table["collisionLayerMask"].value_or(
            static_cast<std::int64_t>(emitter.collisionLayerMask)));
    emitter.trail.trailColorTint = ReadVector4(table["trailColorTint"], emitter.trail.trailColorTint);
    emitter.light.lightColor = ReadVector4(table["lightColor"], emitter.light.lightColor);
    ReadCurve(table, "sizeCurve", emitter.sizeCurve);
    ReadCurve(table, "velocityCurve", emitter.velocityCurve);
    ReadCurve(table, "rotationCurve", emitter.rotationCurve);
    ReadCurve(table, "dragCurve", emitter.dragCurve);
    ReadCurve(table, "temperatureCurve", emitter.temperatureCurve);
    ReadCurve(table, "emitRateCurve", emitter.emitRateCurve);
    ReadCurve(table, "speedSizeCurve", emitter.speedSizeCurve);
    DeserializeParticleGradient(table, "colorGradient", emitter.colorGradient);
    DeserializeParticleGradient(table, "speedColorGradient", emitter.speedColorGradient);
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

    // 内蔵の力。キーがあれば新形式、無ければ旧フィールドから組み立てる。
    // 「空のリストを保存した」と「旧ファイル」は区別が要る。前者は力ゼロが意図なので、
    // キーの有無で判定する (要素数では区別できない)。
    if (const auto* forces = table["localForces"].as_array()) {
        emitter.localForces.clear();
        for (const auto& node : *forces) {
            if (const auto* item = node.as_table()) emitter.localForces.push_back(ReadForce(*item));
        }
    } else {
        MigrateLegacyForces(table, emitter);
    }

    // ランタイム状態はここでは触れない (この型が持っていない)。
    // コンポーネントへ流し込んだ呼び出し側が ResetPlayback() で初期化する。
}

} // namespace fbzz::asset
