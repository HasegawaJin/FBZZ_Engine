/// @file    FluidRecipeCodec.cpp
/// @brief   .fluid の TOML 読み書きと旧形式の移行。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/Fluid/Import/FluidRecipeCodec.hpp>

#include "FluidRecipeLabels.hpp"

#include <Engine/Util/Files/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::asset {
namespace {

using namespace fbzz::fluid;
namespace labels = fluid_recipe_labels;

using NodeView = toml::node_view<const toml::node>;

/// @note 版 1 は発生源を [[gas_source]] / [[liquid_emitter]] に分けて保存する。
constexpr int64_t FORMAT_VERSION = 2;

template <typename Enum, std::size_t LABEL_COUNT>
const char* LabelForEnum(Enum value, const char* const (&labels)[LABEL_COUNT])
{
    const auto index = static_cast<std::size_t>(value);
    return labels[index < LABEL_COUNT ? index : 0];
}

/// @note 知らない名前は既定 (添字 0) に落とす。新しい版で増えた種類を古いビルドで開いても読めるようにする。
template <typename Enum, std::size_t LABEL_COUNT>
Enum EnumFromLabel(std::string_view name, const char* const (&labels)[LABEL_COUNT])
{
    for (std::size_t index = 0; index < LABEL_COUNT; ++index)
        if (name == labels[index]) {
            return static_cast<Enum>(index);
        }
    return static_cast<Enum>(0);
}

/// @brief float を «最短往復» の double にする。
/// @note 素の static_cast だと 0.1f が 0.10000000149011612 と書き出され、読んで書き戻すだけで
/// @note .fluid の行が汚れ、AI の 1 項目修正が無関係な行まで動かして見え «同じレシピなら同じ
/// @note バイト列» も崩れる。float として読み戻せる最短の 10 進へ畳んでから書く。
double NormalizedDouble(float value)
{
    char buffer[32]{};
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (written.ec != std::errc{}) {
        return static_cast<double>(value);
    }
    double parsedValue = 0.0;
    const auto parsed = std::from_chars(buffer, written.ptr, parsedValue);
    return parsed.ec == std::errc{} ? parsedValue : static_cast<double>(value);
}

toml::array WriteVector3(const math::Vector3& value)
{
    return toml::array{ NormalizedDouble(value.x), NormalizedDouble(value.y), NormalizedDouble(value.z) };
}

toml::array WriteVector4(const math::Vector4& value)
{
    return toml::array{ NormalizedDouble(value.x), NormalizedDouble(value.y),
                        NormalizedDouble(value.z), NormalizedDouble(value.w) };
}

math::Vector3 ReadVector3(NodeView view, const math::Vector3& fallback)
{
    const toml::array* array = view.as_array();
    if (array == nullptr || array->size() < 3) {
        return fallback;
    }
    return { static_cast<float>((*array)[0].value_or(static_cast<double>(fallback.x))),
             static_cast<float>((*array)[1].value_or(static_cast<double>(fallback.y))),
             static_cast<float>((*array)[2].value_or(static_cast<double>(fallback.z))) };
}

math::Vector4 ReadVector4(NodeView view, const math::Vector4& fallback)
{
    const toml::array* array = view.as_array();
    if (array == nullptr || array->size() < 4) {
        return fallback;
    }
    return { static_cast<float>((*array)[0].value_or(static_cast<double>(fallback.x))),
             static_cast<float>((*array)[1].value_or(static_cast<double>(fallback.y))),
             static_cast<float>((*array)[2].value_or(static_cast<double>(fallback.z))),
             static_cast<float>((*array)[3].value_or(static_cast<double>(fallback.w))) };
}

/// @note 欠けたキーは今の値のまま残す。古い .fluid に新しい設定が無くても既定値で開ける。
class TomlFieldReader {
public:
    explicit TomlFieldReader(NodeView table) : m_table(table) {}

    void Float(const char* key, float& value) const
    {
        value = static_cast<float>(m_table[key].value_or(static_cast<double>(value)));
    }
    void Int(const char* key, int& value) const
    {
        value = static_cast<int>(m_table[key].value_or(static_cast<int64_t>(value)));
    }
    void Bool(const char* key, bool& value) const { value = m_table[key].value_or(value); }
    void Text(const char* key, std::string& value) const
    {
        if (const auto text = m_table[key].value<std::string>()) {
            value = *text;
        }
    }
    void Vec3(const char* key, math::Vector3& value) const { value = ReadVector3(m_table[key], value); }
    void Vec4(const char* key, math::Vector4& value) const { value = ReadVector4(m_table[key], value); }

    template <typename Enum, std::size_t LABEL_COUNT>
    void EnumValue(const char* key, Enum& value, const char* const (&labels)[LABEL_COUNT]) const
    {
        const auto label = m_table[key].value_or(std::string(LabelForEnum(value, labels)));
        value = EnumFromLabel<Enum>(label, labels);
    }

private:
    NodeView m_table;
};

struct TomlFieldWriter {
    toml::table table;
    void Float(const char* key, float value) { table.insert(key, NormalizedDouble(value)); }
    void Int(const char* key, int value) { table.insert(key, static_cast<int64_t>(value)); }
    void Bool(const char* key, bool value) { table.insert(key, value); }
    void Text(const char* key, const std::string& value) { table.insert(key, value); }
    void Vec3(const char* key, const math::Vector3& value) { table.insert(key, WriteVector3(value)); }
    void Vec4(const char* key, const math::Vector4& value) { table.insert(key, WriteVector4(value)); }

    template <typename Enum, std::size_t LABEL_COUNT>
    void EnumValue(const char* key, Enum value, const char* const (&labels)[LABEL_COUNT])
    {
        table.insert(key, std::string(LabelForEnum(value, labels)));
    }
};

/// @note 表の配列を maxCount 個まで読む。表でない要素は飛ばす。
template <typename T, typename ReadElement>
void ReadTableArray(NodeView view, std::size_t maxCount, std::vector<T>& items, ReadElement readElement)
{
    const toml::array* array = view.as_array();
    if (array == nullptr) {
        return;
    }
    for (const toml::node& node : *array) {
        if (items.size() >= maxCount) {
            break;
        }
        if (const toml::table* table = node.as_table()) {
            items.push_back(readElement(NodeView{ table }));
        }
    }
}

/// @brief 表の配列を書く。空なら何も書かない。
/// @note «key = []» は表の配列ではなく値の葉として読まれ、要素が無ければ項目も出さない
/// @note Reflect の一覧 (FluidRecipeBakeTests) と食い違うため。
template <typename T, typename WriteElement>
void WriteTableArray(toml::table& parent, const char* key, const std::vector<T>& items, WriteElement writeElement)
{
    if (items.empty()) {
        return;
    }
    toml::array array;
    for (const T& item : items) {
        array.push_back(writeElement(item));
    }
    parent.insert(key, std::move(array));
}

void ReadGas(NodeView view, FluidGasSettings& gas)
{
    const TomlFieldReader reader{ view };
    reader.Int("resolution", gas.resolution);
    reader.Float("buoyancy", gas.buoyancy);
    reader.Float("weight", gas.weight);
    reader.Float("vorticity", gas.vorticity);
    reader.Float("turbulence", gas.turbulence);
    reader.Float("turbulence_scale", gas.turbulenceScale);
    reader.Float("density_dissipation", gas.densityDissipation);
    reader.Float("temperature_dissipation", gas.temperatureDissipation);
    reader.Float("velocity_damping", gas.velocityDamping);
    reader.Float("ignition_temperature", gas.ignitionTemperature);
    reader.Float("burn_rate", gas.burnRate);
    reader.Float("burn_heat", gas.burnHeat);
    reader.Float("burn_smoke", gas.burnSmoke);
    reader.Float("burn_expansion", gas.burnExpansion);
    reader.Vec3("wind", gas.wind);
    reader.Bool("floor", gas.floor);
    reader.Int("pressure_iterations", gas.pressureIterations);
    reader.Bool("sharp_advection", gas.sharpAdvection);
    reader.Float("detail_period", gas.detailPeriod);
}

toml::table WriteGas(const FluidGasSettings& gas)
{
    TomlFieldWriter writer;
    writer.Int("resolution", gas.resolution);
    writer.Float("buoyancy", gas.buoyancy);
    writer.Float("weight", gas.weight);
    writer.Float("vorticity", gas.vorticity);
    writer.Float("turbulence", gas.turbulence);
    writer.Float("turbulence_scale", gas.turbulenceScale);
    writer.Float("density_dissipation", gas.densityDissipation);
    writer.Float("temperature_dissipation", gas.temperatureDissipation);
    writer.Float("velocity_damping", gas.velocityDamping);
    writer.Float("ignition_temperature", gas.ignitionTemperature);
    writer.Float("burn_rate", gas.burnRate);
    writer.Float("burn_heat", gas.burnHeat);
    writer.Float("burn_smoke", gas.burnSmoke);
    writer.Float("burn_expansion", gas.burnExpansion);
    writer.Vec3("wind", gas.wind);
    writer.Bool("floor", gas.floor);
    writer.Int("pressure_iterations", gas.pressureIterations);
    writer.Bool("sharp_advection", gas.sharpAdvection);
    writer.Float("detail_period", gas.detailPeriod);
    return std::move(writer.table);
}

void ReadLiquid(NodeView view, FluidLiquidSettings& liquid)
{
    const TomlFieldReader reader{ view };
    reader.Int("max_particles", liquid.maxParticles);
    reader.Float("particle_radius", liquid.particleRadius);
    reader.Float("gravity", liquid.gravity);
    reader.Float("viscosity", liquid.viscosity);
    reader.Float("cohesion", liquid.cohesion);
    reader.Int("solver_iterations", liquid.solverIterations);
    reader.Bool("floor", liquid.floor);
    reader.Float("floor_height", liquid.floorHeight);
    reader.Float("floor_friction", liquid.floorFriction);
    reader.Float("particle_lifetime", liquid.particleLifetime);
}

toml::table WriteLiquid(const FluidLiquidSettings& liquid)
{
    TomlFieldWriter writer;
    writer.Int("max_particles", liquid.maxParticles);
    writer.Float("particle_radius", liquid.particleRadius);
    writer.Float("gravity", liquid.gravity);
    writer.Float("viscosity", liquid.viscosity);
    writer.Float("cohesion", liquid.cohesion);
    writer.Int("solver_iterations", liquid.solverIterations);
    writer.Bool("floor", liquid.floor);
    writer.Float("floor_height", liquid.floorHeight);
    writer.Float("floor_friction", liquid.floorFriction);
    writer.Float("particle_lifetime", liquid.particleLifetime);
    return std::move(writer.table);
}

/// @name 部品 (発生源・力・動き)

void ReadMotion(NodeView view, FluidMotion& motion)
{
    const TomlFieldReader reader{ view };
    reader.Bool("inherit_velocity", motion.inheritVelocity);
    motion.keys.clear();
    ReadTableArray(view["key"], static_cast<std::size_t>(kMaxFluidMotionKeys), motion.keys, [](NodeView element) {
        FluidMotionKey key;
        const TomlFieldReader keyReader{ element };
        keyReader.Float("time", key.time);
        keyReader.Vec3("offset", key.offset);
        return key;
    });
    // 評価 (SampleFluidMotion) は昇順を前提に隣のキーと補間する。手で書いたファイルの並びを信用しない。
    std::stable_sort(motion.keys.begin(), motion.keys.end(),
                     [](const FluidMotionKey& left, const FluidMotionKey& right) { return left.time < right.time; });
}

toml::table WriteMotion(const FluidMotion& motion)
{
    TomlFieldWriter writer;
    writer.Bool("inherit_velocity", motion.inheritVelocity);
    WriteTableArray(writer.table, "key", motion.keys, [](const FluidMotionKey& key) {
        TomlFieldWriter keyWriter;
        keyWriter.Float("time", key.time);
        keyWriter.Vec3("offset", key.offset);
        return std::move(keyWriter.table);
    });
    return std::move(writer.table);
}

void ReadAmount(NodeView view, FluidAmount& amount)
{
    amount.keys.clear();
    ReadTableArray(view["key"], static_cast<std::size_t>(kMaxFluidAmountKeys), amount.keys, [](NodeView element) {
        FluidAmountKey key;
        const TomlFieldReader keyReader{ element };
        keyReader.Float("time", key.time);
        keyReader.Float("scale", key.scale);
        return key;
    });
    // 評価 (SampleFluidAmount) は昇順を前提に隣のキーと補間する。手で書いたファイルの並びを信用しない。
    std::stable_sort(amount.keys.begin(), amount.keys.end(),
                     [](const FluidAmountKey& left, const FluidAmountKey& right) { return left.time < right.time; });
}

/// @note キーが 1 つも無ければ節ごと書かない (WriteTableArray が空の配列を書かないのと同じ理由に加えて、
/// @note «倍率 1» を意味する空の [source.amount] を既存の全ファイルへ増やさないため)。
void WriteAmountInto(toml::table& parent, const FluidAmount& amount)
{
    TomlFieldWriter writer;
    WriteTableArray(writer.table, "key", amount.keys, [](const FluidAmountKey& key) {
        TomlFieldWriter keyWriter;
        keyWriter.Float("time", key.time);
        keyWriter.Float("scale", key.scale);
        return std::move(keyWriter.table);
    });
    if (writer.table.empty()) {
        return;
    }
    parent.insert("amount", std::move(writer.table));
}

FluidSource ReadSource(NodeView view)
{
    FluidSource source;
    const TomlFieldReader reader{ view };
    reader.Bool("enabled", source.enabled);
    reader.Text("name", source.name);
    reader.EnumValue("shape", source.shape, labels::SOURCE_SHAPE);
    reader.Vec3("center", source.center);
    reader.Vec3("size", source.size);
    reader.Vec3("direction", source.direction);
    reader.Text("texture", source.texture);
    reader.Float("density", source.density);
    reader.Float("temperature", source.temperature);
    reader.Float("fuel", source.fuel);
    reader.Float("noise", source.noise);
    reader.Vec3("velocity", source.velocity);
    reader.Float("start_time", source.startTime);
    reader.Float("duration", source.duration);
    reader.Float("color_key", source.colorKey);
    reader.Float("spread", source.spread);
    reader.Int("count", source.count);
    ReadMotion(view["motion"], source.motion);
    ReadAmount(view["amount"], source.amount);
    return source;
}

toml::table WriteSource(const FluidSource& source)
{
    TomlFieldWriter writer;
    writer.Bool("enabled", source.enabled);
    writer.Text("name", source.name);
    writer.EnumValue("shape", source.shape, labels::SOURCE_SHAPE);
    writer.Vec3("center", source.center);
    writer.Vec3("size", source.size);
    writer.Vec3("direction", source.direction);
    writer.Text("texture", source.texture);
    writer.Float("density", source.density);
    writer.Float("temperature", source.temperature);
    writer.Float("fuel", source.fuel);
    writer.Float("noise", source.noise);
    writer.Vec3("velocity", source.velocity);
    writer.Float("start_time", source.startTime);
    writer.Float("duration", source.duration);
    writer.Float("color_key", source.colorKey);
    writer.Float("spread", source.spread);
    writer.Int("count", source.count);
    writer.table.insert("motion", WriteMotion(source.motion));
    WriteAmountInto(writer.table, source.amount);
    return std::move(writer.table);
}

FluidForce ReadForce(NodeView view)
{
    FluidForce force;
    const TomlFieldReader reader{ view };
    reader.Bool("enabled", force.enabled);
    reader.Text("name", force.name);
    reader.EnumValue("type", force.type, labels::FORCE_TYPE);
    reader.Vec3("center", force.center);
    reader.Vec3("direction", force.direction);
    reader.Float("strength", force.strength);
    reader.Float("radius", force.radius);
    reader.Float("falloff_power", force.falloffPower);
    reader.Float("noise_frequency", force.noiseFrequency);
    reader.Float("noise_speed", force.noiseSpeed);
    reader.Float("start_time", force.startTime);
    reader.Float("duration", force.duration);
    ReadMotion(view["motion"], force.motion);
    ReadAmount(view["amount"], force.amount);
    return force;
}

toml::table WriteForce(const FluidForce& force)
{
    TomlFieldWriter writer;
    writer.Bool("enabled", force.enabled);
    writer.Text("name", force.name);
    writer.EnumValue("type", force.type, labels::FORCE_TYPE);
    writer.Vec3("center", force.center);
    writer.Vec3("direction", force.direction);
    writer.Float("strength", force.strength);
    writer.Float("radius", force.radius);
    writer.Float("falloff_power", force.falloffPower);
    writer.Float("noise_frequency", force.noiseFrequency);
    writer.Float("noise_speed", force.noiseSpeed);
    writer.Float("start_time", force.startTime);
    writer.Float("duration", force.duration);
    writer.table.insert("motion", WriteMotion(force.motion));
    WriteAmountInto(writer.table, force.amount);
    return std::move(writer.table);
}

FluidCollider ReadCollider(NodeView view)
{
    FluidCollider collider;
    const TomlFieldReader reader{ view };
    reader.Bool("enabled", collider.enabled);
    reader.Text("name", collider.name);
    reader.EnumValue("shape", collider.shape, labels::COLLIDER_SHAPE);
    reader.Vec3("center", collider.center);
    reader.Vec3("size", collider.size);
    reader.Vec3("direction", collider.direction);
    reader.Float("friction", collider.friction);
    reader.Float("start_time", collider.startTime);
    reader.Float("duration", collider.duration);
    ReadMotion(view["motion"], collider.motion);
    return collider;
}

toml::table WriteCollider(const FluidCollider& collider)
{
    TomlFieldWriter writer;
    writer.Bool("enabled", collider.enabled);
    writer.Text("name", collider.name);
    writer.EnumValue("shape", collider.shape, labels::COLLIDER_SHAPE);
    writer.Vec3("center", collider.center);
    writer.Vec3("size", collider.size);
    writer.Vec3("direction", collider.direction);
    writer.Float("friction", collider.friction);
    writer.Float("start_time", collider.startTime);
    writer.Float("duration", collider.duration);
    writer.table.insert("motion", WriteMotion(collider.motion));
    return std::move(writer.table);
}

/// @name version 1 からの移行

FluidSource ReadV1GasSource(NodeView view)
{
    FluidSource source;
    const TomlFieldReader reader{ view };
    reader.EnumValue("shape", source.shape, labels::SOURCE_SHAPE);
    reader.Vec3("center", source.center);
    reader.Vec3("size", source.size);
    reader.Float("density", source.density);
    reader.Float("temperature", source.temperature);
    reader.Float("fuel", source.fuel);
    reader.Vec3("velocity", source.velocity);
    reader.Float("start_time", source.startTime);
    reader.Float("duration", source.duration);
    reader.Float("noise", source.noise);
    return source;
}

FluidSource ReadV1LiquidEmitter(NodeView view)
{
    // version 1 の液体の発生源は既定値が今の FluidSource と違う。欠けたキーを今の既定で埋めると、
    // キーを省いて書いた古いファイルが «開いただけで別の絵» になる。当時の既定から読み始める。
    FluidSource source;
    source.shape = FluidSourceShape::Sphere;
    source.center = { 0.0f, -0.6f, 0.0f };
    source.velocity = { 0.0f, 2.5f, 0.0f };
    source.spread = 0.5f;
    source.count = 600;
    source.startTime = 0.0f;
    source.duration = 0.15f;
    float radius = 0.08f;
    const TomlFieldReader reader{ view };
    reader.Vec3("position", source.center);
    reader.Float("radius", radius);
    reader.Vec3("velocity", source.velocity);
    reader.Float("spread", source.spread);
    reader.Int("count", source.count);
    reader.Float("start_time", source.startTime);
    reader.Float("duration", source.duration);
    source.size = { radius, radius, radius };
    return source;
}

void ReadRamp(NodeView view, FluidColorRamp& ramp)
{
    const toml::array* array = view.as_array();
    if (array == nullptr) {
        return;
    }
    const std::size_t count = (std::min)(array->size(), ramp.stops.size());
    for (std::size_t index = 0; index < count; ++index) {
        const toml::table* table = (*array)[index].as_table();
        if (table == nullptr) {
            continue;
        }
        const TomlFieldReader reader{ NodeView{ table } };
        reader.Vec3("color", ramp.stops[index].color);
        reader.Float("position", ramp.stops[index].position);
    }
    // 評価は昇順を前提に隣の点と補間する (VolumeColorRamp と同じ規則)。
    std::stable_sort(ramp.stops.begin(), ramp.stops.end(),
                     [](const FluidColorStop& left, const FluidColorStop& right) { return left.position < right.position; });
}

toml::array WriteRamp(const FluidColorRamp& ramp)
{
    toml::array array;
    for (const FluidColorStop& stop : ramp.stops) {
        TomlFieldWriter writer;
        writer.Vec3("color", stop.color);
        writer.Float("position", stop.position);
        array.push_back(std::move(writer.table));
    }
    return array;
}

void ReadRender(NodeView view, FluidRenderSettings& renderSettings)
{
    const TomlFieldReader reader{ view };
    reader.EnumValue("shading", renderSettings.shading, labels::SHADING);
    reader.Vec4("smoke_color", renderSettings.smokeColor);
    reader.Vec4("shadow_color", renderSettings.shadowColor);
    reader.Float("opacity", renderSettings.opacity);
    reader.Float("self_shadow", renderSettings.selfShadow);
    reader.Vec3("light_direction", renderSettings.lightDirection);
    reader.Float("detail_strength", renderSettings.detailStrength);
    reader.Float("detail_scale", renderSettings.detailScale);
    reader.Float("fire_kelvin", renderSettings.fireKelvin);
    reader.Float("fire_intensity", renderSettings.fireIntensity);
    reader.Bool("use_emission_ramp", renderSettings.useEmissionRamp);
    ReadRamp(view["emission_ramp"], renderSettings.emissionRamp);
    reader.Bool("use_albedo_ramp", renderSettings.useAlbedoRamp);
    ReadRamp(view["albedo_ramp"], renderSettings.albedoRamp);
    reader.Vec4("liquid_color", renderSettings.liquidColor);
    reader.Float("liquid_radius_scale", renderSettings.liquidRadiusScale);
    reader.Float("liquid_threshold", renderSettings.liquidThreshold);
    reader.Float("specular", renderSettings.specular);
    reader.Float("liquid_softness", renderSettings.liquidSoftness);
    reader.Float("liquid_extinction", renderSettings.liquidExtinction);
    reader.Float("liquid_gloss", renderSettings.liquidGloss);
    reader.Float("liquid_fresnel", renderSettings.liquidFresnel);
}

toml::table WriteRender(const FluidRenderSettings& renderSettings)
{
    TomlFieldWriter writer;
    writer.EnumValue("shading", renderSettings.shading, labels::SHADING);
    writer.Vec4("smoke_color", renderSettings.smokeColor);
    writer.Vec4("shadow_color", renderSettings.shadowColor);
    writer.Float("opacity", renderSettings.opacity);
    writer.Float("self_shadow", renderSettings.selfShadow);
    writer.Vec3("light_direction", renderSettings.lightDirection);
    writer.Float("detail_strength", renderSettings.detailStrength);
    writer.Float("detail_scale", renderSettings.detailScale);
    writer.Float("fire_kelvin", renderSettings.fireKelvin);
    writer.Float("fire_intensity", renderSettings.fireIntensity);
    writer.Bool("use_emission_ramp", renderSettings.useEmissionRamp);
    writer.table.insert("emission_ramp", WriteRamp(renderSettings.emissionRamp));
    writer.Bool("use_albedo_ramp", renderSettings.useAlbedoRamp);
    writer.table.insert("albedo_ramp", WriteRamp(renderSettings.albedoRamp));
    writer.Vec4("liquid_color", renderSettings.liquidColor);
    writer.Float("liquid_radius_scale", renderSettings.liquidRadiusScale);
    writer.Float("liquid_threshold", renderSettings.liquidThreshold);
    writer.Float("specular", renderSettings.specular);
    writer.Float("liquid_softness", renderSettings.liquidSoftness);
    writer.Float("liquid_extinction", renderSettings.liquidExtinction);
    writer.Float("liquid_gloss", renderSettings.liquidGloss);
    writer.Float("liquid_fresnel", renderSettings.liquidFresnel);
    return std::move(writer.table);
}

void ReadOutput(NodeView view, FluidOutputSettings& output)
{
    const TomlFieldReader reader{ view };
    reader.Int("frame_size", output.frameSize);
    reader.Int("columns", output.columns);
    reader.Int("rows", output.rows);
    reader.Int("supersampling", output.supersampling);
    reader.Float("duration", output.duration);
    reader.Float("warmup", output.warmup);
    reader.Int("substeps", output.substeps);
    reader.Bool("loop", output.loop);
    reader.Float("loop_blend_fraction", output.loopBlendFraction);
    reader.Bool("motion_vectors", output.motionVectors);
    reader.Bool("vector_field", output.vectorField);
    reader.Int("vector_field_resolution", output.vectorFieldResolution);
    reader.Vec3("vector_field_extents", output.vectorFieldExtents);
}

toml::table WriteOutput(const FluidOutputSettings& output)
{
    TomlFieldWriter writer;
    writer.Int("frame_size", output.frameSize);
    writer.Int("columns", output.columns);
    writer.Int("rows", output.rows);
    writer.Int("supersampling", output.supersampling);
    writer.Float("duration", output.duration);
    writer.Float("warmup", output.warmup);
    writer.Int("substeps", output.substeps);
    writer.Bool("loop", output.loop);
    writer.Float("loop_blend_fraction", output.loopBlendFraction);
    writer.Bool("motion_vectors", output.motionVectors);
    writer.Bool("vector_field", output.vectorField);
    writer.Int("vector_field_resolution", output.vectorFieldResolution);
    writer.Vec3("vector_field_extents", output.vectorFieldExtents);
    return std::move(writer.table);
}

void ReadBake(NodeView view, FluidBakeSettings& bake)
{
    const TomlFieldReader reader{ view };
    reader.EnumValue("mode", bake.mode, labels::BAKE_MODE);
    reader.Int("volume_resolution", bake.volumeResolution);
    reader.Int("ray_steps", bake.raySteps);
    reader.Int("shadow_steps", bake.shadowSteps);
    reader.EnumValue("solver", bake.solver, labels::BAKE_SOLVER);
    reader.Float("density_scale", bake.densityScale);
    reader.Int("scattering_octaves", bake.scatteringOctaves);
    reader.Float("sky_occlusion", bake.skyOcclusion);
    reader.Bool("blackbody_emission", bake.blackbodyEmission);
    reader.Float("blackbody_min_kelvin", bake.blackbodyMinKelvin);
    reader.Float("blackbody_max_kelvin", bake.blackbodyMaxKelvin);
    reader.Bool("six_way_lightmaps", bake.sixWayLightmaps);
    reader.Float("light_yaw_degrees", bake.lightYawDegrees);
    reader.Float("light_pitch_degrees", bake.lightPitchDegrees);
    reader.Vec3("light_color", bake.lightColor);
    reader.Vec3("ambient", bake.ambient);
    reader.Float("extinction", bake.extinction);
    reader.Float("anisotropy", bake.anisotropy);
    reader.Float("emission_intensity", bake.emissionIntensity);
    reader.Float("exposure", bake.exposure);
    reader.Float("camera_yaw_degrees", bake.cameraYawDegrees);
    reader.Float("half_extent", bake.halfExtent);
}

toml::table WriteBake(const FluidBakeSettings& bake)
{
    TomlFieldWriter writer;
    writer.EnumValue("mode", bake.mode, labels::BAKE_MODE);
    writer.Int("volume_resolution", bake.volumeResolution);
    writer.Int("ray_steps", bake.raySteps);
    writer.Int("shadow_steps", bake.shadowSteps);
    writer.EnumValue("solver", bake.solver, labels::BAKE_SOLVER);
    writer.Float("density_scale", bake.densityScale);
    writer.Int("scattering_octaves", bake.scatteringOctaves);
    writer.Float("sky_occlusion", bake.skyOcclusion);
    writer.Bool("blackbody_emission", bake.blackbodyEmission);
    writer.Float("blackbody_min_kelvin", bake.blackbodyMinKelvin);
    writer.Float("blackbody_max_kelvin", bake.blackbodyMaxKelvin);
    writer.Bool("six_way_lightmaps", bake.sixWayLightmaps);
    writer.Float("light_yaw_degrees", bake.lightYawDegrees);
    writer.Float("light_pitch_degrees", bake.lightPitchDegrees);
    writer.Vec3("light_color", bake.lightColor);
    writer.Vec3("ambient", bake.ambient);
    writer.Float("extinction", bake.extinction);
    writer.Float("anisotropy", bake.anisotropy);
    writer.Float("emission_intensity", bake.emissionIntensity);
    writer.Float("exposure", bake.exposure);
    writer.Float("camera_yaw_degrees", bake.cameraYawDegrees);
    writer.Float("half_extent", bake.halfExtent);
    return std::move(writer.table);
}

void ReadRecipeParts(NodeView root, int64_t version, FluidRecipe& recipe)
{
    const auto maxSources = static_cast<std::size_t>(kMaxFluidSources);
    if (version < 2) {
        // 版 1 は保存時の kind の発生源だけを移し、非表示だった発生源の混入を防ぐ。
        if (recipe.kind == FluidKind::Liquid) {
            ReadTableArray(root["liquid_emitter"], maxSources, recipe.sources, ReadV1LiquidEmitter);
        } else {
            ReadTableArray(root["gas_source"], maxSources, recipe.sources, ReadV1GasSource);
        }
        return;
    }

    ReadTableArray(root["source"], maxSources, recipe.sources, ReadSource);
    ReadTableArray(root["force"], static_cast<std::size_t>(kMaxFluidForces), recipe.forces, ReadForce);
    ReadTableArray(root["collider"], static_cast<std::size_t>(kMaxFluidColliders), recipe.colliders, ReadCollider);
}

void AddEmptyKeys(toml::table& part, const char* sectionName)
{
    auto* section = part.insert(sectionName, toml::table{}).first->second.as_table();
    if (section != nullptr) {
        section->insert("key", toml::array{});
    }
}

/// @note 編集 API は空のリストやキーフレーム配列も編集先として必要とする。
void AddEditablePartDefaults(toml::table& root, const char* partName, bool hasAmount)
{
    auto* parts = root.insert(partName, toml::array{}).first->second.as_array();
    if (parts == nullptr) {
        return;
    }

    for (auto& node : *parts) {
        auto* part = node.as_table();
        if (part == nullptr) {
            continue;
        }
        AddEmptyKeys(*part, "motion");
        if (hasAmount) {
            AddEmptyKeys(*part, "amount");
        }
    }
}

} // namespace

bool LoadFluidRecipe(const std::string& absPath, FluidRecipe& outRecipe, std::string* outError)
{
    std::ifstream stream(util::FileSystem::PathFromUtf8(absPath), std::ios::binary);
    if (!stream) {
        if (outError != nullptr) {
            *outError = "開けません: " + absPath;
        }
        return false;
    }
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return LoadFluidRecipeFromToml(buffer.str(), outRecipe, outError);
}

bool LoadFluidRecipeFromToml(std::string_view text, FluidRecipe& outRecipe, std::string* outError)
{
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        if (outError != nullptr) {
            *outError = std::string(parsed.error().description());
        }
        return false;
    }
    const toml::table& root = parsed.table();
    const NodeView view{ &root };

    FluidRecipe recipe;
    const int64_t version = view["version"].value_or(static_cast<int64_t>(1));
    const TomlFieldReader reader{ view };
    reader.EnumValue("kind", recipe.kind, labels::KIND);
    recipe.seed = static_cast<uint32_t>(view["seed"].value_or(static_cast<int64_t>(recipe.seed)));
    ReadGas(view["gas"], recipe.gas);
    ReadLiquid(view["liquid"], recipe.liquid);
    ReadRender(view["render"], recipe.render);
    ReadOutput(view["output"], recipe.output);
    ReadBake(view["bake"], recipe.bake);
    ReadRecipeParts(view, version, recipe);
    outRecipe = std::move(recipe);
    return true;
}

bool SaveFluidRecipe(const std::string& absPath, const FluidRecipe& recipe)
{
    return util::FileSystem::WriteText(absPath, SerializeFluidRecipe(recipe));
}

std::string SerializeFluidRecipe(const FluidRecipe& recipe, bool explicitDefaults)
{
    toml::table root;
    root.insert("version", FORMAT_VERSION);
    root.insert("kind", std::string(LabelForEnum(recipe.kind, labels::KIND)));
    root.insert("seed", static_cast<int64_t>(recipe.seed));
    root.insert("gas", WriteGas(recipe.gas));
    root.insert("liquid", WriteLiquid(recipe.liquid));
    root.insert("render", WriteRender(recipe.render));
    root.insert("output", WriteOutput(recipe.output));
    root.insert("bake", WriteBake(recipe.bake));
    WriteTableArray(root, "source", recipe.sources, WriteSource);
    WriteTableArray(root, "force", recipe.forces, WriteForce);
    WriteTableArray(root, "collider", recipe.colliders, WriteCollider);

    if (explicitDefaults) {
        AddEditablePartDefaults(root, "source", true);
        AddEditablePartDefaults(root, "force", true);
        AddEditablePartDefaults(root, "collider", false);
    }
    std::ostringstream serializedRecipe;
    serializedRecipe << root << '\n';
    return serializedRecipe.str();
}

} // namespace fbzz::asset
