/// @file    FluidRecipeCodec.cpp
/// @brief   .fluid の TOML 読み書きとプリセット
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/FluidRecipeCodec.hpp>

#include <Engine/Scene/Script.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

/// @note この翻訳単位は «設定型そのもの» を端から端まで舐めるので、全項目を fluid:: で
///       修飾すると型より修飾のほうが長くなる。読み書きの対象が FBZZFluid の型しか
///       無いことが自明なため、ここだけ持ち込む (ヘッダーでは決してやらない)。
using namespace fbzz::fluid;

namespace fbzz::asset {
namespace {

using NodeView = toml::node_view<const toml::node>;

/// 保存する形式の版。1 は発生源を気体 [[gas_source]] / 液体 [[liquid_emitter]] の 2 本で持っていた。
constexpr int64_t kFormatVersion = 2;

const char* KindName(FluidKind kind)
{
    return kind == FluidKind::Liquid ? "liquid" : "gas";
}

FluidKind KindFromName(std::string_view name)
{
    return name == "liquid" ? FluidKind::Liquid : FluidKind::Gas;
}

const char* ShadingName(FluidShading shading)
{
    switch (shading) {
    case FluidShading::Fire:       return "fire";
    case FluidShading::Glow:       return "glow";
    case FluidShading::Distortion: return "distortion";
    case FluidShading::Liquid:     return "liquid";
    case FluidShading::Smoke:
    default:                       return "smoke";
    }
}

FluidShading ShadingFromName(std::string_view name)
{
    if (name == "fire")       return FluidShading::Fire;
    if (name == "glow")       return FluidShading::Glow;
    if (name == "distortion") return FluidShading::Distortion;
    if (name == "liquid")     return FluidShading::Liquid;
    return FluidShading::Smoke;
}

/// ReflectFluidRecipe の Enum の選択肢。並びは enum の値、文字列は TOML に書く名前と同じにする
/// (JSON は添字で運び、TOML は名前で書く。両者の対応はテストで縛る)。
constexpr const char* kKindLabels[] = { "gas", "liquid" };
constexpr const char* kShadingLabels[] = { "smoke", "fire", "glow", "distortion", "liquid" };
constexpr const char* kShapeLabels[] = { "sphere", "box", "cone", "ring", "texture", "capsule", "cylinder" };
constexpr const char* kForceTypeLabels[] = { "wind", "attract", "repulse", "vortex", "noise", "drag" };
constexpr const char* kColliderShapeLabels[] = { "sphere", "box", "plane", "capsule", "cylinder" };
constexpr const char* kBakeModeLabels[] = { "2d", "3d" };
constexpr const char* kBakeSolverLabels[] = { "auto", "gpu", "cpu" };

template <typename E, std::size_t N>
const char* LabelOf(E value, const char* const (&labels)[N])
{
    const auto index = static_cast<std::size_t>(value);
    return labels[index < N ? index : 0];
}

/// 知らない名前は既定 (添字 0) に落とす。新しい版で増えた種類を古いビルドで開いても読めるようにする。
template <typename E, std::size_t N>
E FromLabel(std::string_view name, const char* const (&labels)[N])
{
    for (std::size_t index = 0; index < N; ++index)
        if (name == labels[index]) return static_cast<E>(index);
    return static_cast<E>(0);
}

const char* BakeModeName(FluidBakeMode mode)
{
    return kBakeModeLabels[mode == FluidBakeMode::Volume3D ? 1 : 0];
}

FluidBakeMode BakeModeFromName(std::string_view name)
{
    return name == kBakeModeLabels[1] ? FluidBakeMode::Volume3D : FluidBakeMode::Flat2D;
}

const char* BakeSolverName(FluidBakeSolver solver)
{
    return LabelOf(solver, kBakeSolverLabels);
}

FluidBakeSolver BakeSolverFromName(std::string_view name)
{
    return FromLabel<FluidBakeSolver>(name, kBakeSolverLabels);
}

/// @brief float を «最短往復» の double にする。
/// @note 素の static_cast だと 0.1f が 0.10000000149011612 と書き出され、読んで書き戻すだけで
///       .fluid の行が汚れ、AI の 1 項目修正が無関係な行まで動かして見え «同じレシピなら同じ
///       バイト列» も崩れる。float として読み戻せる最短の 10 進へ畳んでから書く。
double NormalizedDouble(float value)
{
    char buffer[32]{};
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (written.ec != std::errc{}) return static_cast<double>(value);
    double out = 0.0;
    const auto parsed = std::from_chars(buffer, written.ptr, out);
    return parsed.ec == std::errc{} ? out : static_cast<double>(value);
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
    if (array == nullptr || array->size() < 3) return fallback;
    return { static_cast<float>((*array)[0].value_or(static_cast<double>(fallback.x))),
             static_cast<float>((*array)[1].value_or(static_cast<double>(fallback.y))),
             static_cast<float>((*array)[2].value_or(static_cast<double>(fallback.z))) };
}

math::Vector4 ReadVector4(NodeView view, const math::Vector4& fallback)
{
    const toml::array* array = view.as_array();
    if (array == nullptr || array->size() < 4) return fallback;
    return { static_cast<float>((*array)[0].value_or(static_cast<double>(fallback.x))),
             static_cast<float>((*array)[1].value_or(static_cast<double>(fallback.y))),
             static_cast<float>((*array)[2].value_or(static_cast<double>(fallback.z))),
             static_cast<float>((*array)[3].value_or(static_cast<double>(fallback.w))) };
}

/// 欠けたキーは今の値のまま残す。古い .fluid に新しい設定が無くても既定値で開ける。
struct Reader {
    NodeView table;
    void Float(const char* key, float& value) const
    {
        value = static_cast<float>(table[key].value_or(static_cast<double>(value)));
    }
    void Int(const char* key, int& value) const
    {
        value = static_cast<int>(table[key].value_or(static_cast<int64_t>(value)));
    }
    void Bool(const char* key, bool& value) const { value = table[key].value_or(value); }
    void Text(const char* key, std::string& value) const
    {
        if (const auto text = table[key].value<std::string>()) value = *text;
    }
    void Vec3(const char* key, math::Vector3& value) const { value = ReadVector3(table[key], value); }
    void Vec4(const char* key, math::Vector4& value) const { value = ReadVector4(table[key], value); }
};

struct Writer {
    toml::table table;
    void Float(const char* key, float value) { table.insert(key, NormalizedDouble(value)); }
    void Int(const char* key, int value) { table.insert(key, static_cast<int64_t>(value)); }
    void Bool(const char* key, bool value) { table.insert(key, value); }
    void Text(const char* key, const char* value) { table.insert(key, std::string(value)); }
    void Text(const char* key, const std::string& value) { table.insert(key, value); }
    void Vec3(const char* key, const math::Vector3& value) { table.insert(key, WriteVector3(value)); }
    void Vec4(const char* key, const math::Vector4& value) { table.insert(key, WriteVector4(value)); }
};

/// 表の配列を maxCount 個まで読む。表でない要素は飛ばす。
template <typename T, typename ReadElement>
void ReadTableArray(NodeView view, std::size_t maxCount, std::vector<T>& out, ReadElement readElement)
{
    const toml::array* array = view.as_array();
    if (array == nullptr) return;
    for (const toml::node& node : *array) {
        if (out.size() >= maxCount) break;
        if (const toml::table* table = node.as_table()) out.push_back(readElement(NodeView{ table }));
    }
}

/// @brief 表の配列を書く。空なら何も書かない。
/// @note «key = []» は表の配列ではなく値の葉として読まれ、要素が無ければ項目も出さない
///       Reflect の一覧 (FluidRecipeBakeTests) と食い違うため。
template <typename T, typename WriteElement>
void WriteTableArray(toml::table& parent, const char* key, const std::vector<T>& items, WriteElement writeElement)
{
    if (items.empty()) return;
    toml::array array;
    for (const T& item : items) array.push_back(writeElement(item));
    parent.insert(key, std::move(array));
}

void ReadGas(NodeView view, FluidGasSettings& gas)
{
    const Reader r{ view };
    r.Int("resolution", gas.resolution);
    r.Float("buoyancy", gas.buoyancy);
    r.Float("weight", gas.weight);
    r.Float("vorticity", gas.vorticity);
    r.Float("turbulence", gas.turbulence);
    r.Float("turbulence_scale", gas.turbulenceScale);
    r.Float("density_dissipation", gas.densityDissipation);
    r.Float("temperature_dissipation", gas.temperatureDissipation);
    r.Float("velocity_damping", gas.velocityDamping);
    r.Float("ignition_temperature", gas.ignitionTemperature);
    r.Float("burn_rate", gas.burnRate);
    r.Float("burn_heat", gas.burnHeat);
    r.Float("burn_smoke", gas.burnSmoke);
    r.Float("burn_expansion", gas.burnExpansion);
    r.Vec3("wind", gas.wind);
    r.Bool("floor", gas.floor);
    r.Int("pressure_iterations", gas.pressureIterations);
    r.Bool("sharp_advection", gas.sharpAdvection);
    r.Float("detail_period", gas.detailPeriod);
}

toml::table WriteGas(const FluidGasSettings& gas)
{
    Writer w;
    w.Int("resolution", gas.resolution);
    w.Float("buoyancy", gas.buoyancy);
    w.Float("weight", gas.weight);
    w.Float("vorticity", gas.vorticity);
    w.Float("turbulence", gas.turbulence);
    w.Float("turbulence_scale", gas.turbulenceScale);
    w.Float("density_dissipation", gas.densityDissipation);
    w.Float("temperature_dissipation", gas.temperatureDissipation);
    w.Float("velocity_damping", gas.velocityDamping);
    w.Float("ignition_temperature", gas.ignitionTemperature);
    w.Float("burn_rate", gas.burnRate);
    w.Float("burn_heat", gas.burnHeat);
    w.Float("burn_smoke", gas.burnSmoke);
    w.Float("burn_expansion", gas.burnExpansion);
    w.Vec3("wind", gas.wind);
    w.Bool("floor", gas.floor);
    w.Int("pressure_iterations", gas.pressureIterations);
    w.Bool("sharp_advection", gas.sharpAdvection);
    w.Float("detail_period", gas.detailPeriod);
    return std::move(w.table);
}

void ReadLiquid(NodeView view, FluidLiquidSettings& liquid)
{
    const Reader r{ view };
    r.Int("max_particles", liquid.maxParticles);
    r.Float("particle_radius", liquid.particleRadius);
    r.Float("gravity", liquid.gravity);
    r.Float("viscosity", liquid.viscosity);
    r.Float("cohesion", liquid.cohesion);
    r.Int("solver_iterations", liquid.solverIterations);
    r.Bool("floor", liquid.floor);
    r.Float("floor_height", liquid.floorHeight);
    r.Float("floor_friction", liquid.floorFriction);
    r.Float("particle_lifetime", liquid.particleLifetime);
}

toml::table WriteLiquid(const FluidLiquidSettings& liquid)
{
    Writer w;
    w.Int("max_particles", liquid.maxParticles);
    w.Float("particle_radius", liquid.particleRadius);
    w.Float("gravity", liquid.gravity);
    w.Float("viscosity", liquid.viscosity);
    w.Float("cohesion", liquid.cohesion);
    w.Int("solver_iterations", liquid.solverIterations);
    w.Bool("floor", liquid.floor);
    w.Float("floor_height", liquid.floorHeight);
    w.Float("floor_friction", liquid.floorFriction);
    w.Float("particle_lifetime", liquid.particleLifetime);
    return std::move(w.table);
}

/// @name 部品 (発生源・力・動き)

void ReadMotion(NodeView view, FluidMotion& motion)
{
    const Reader r{ view };
    r.Bool("inherit_velocity", motion.inheritVelocity);
    motion.keys.clear();
    ReadTableArray(view["key"], static_cast<std::size_t>(kMaxFluidMotionKeys), motion.keys, [](NodeView element) {
        FluidMotionKey key;
        const Reader k{ element };
        k.Float("time", key.time);
        k.Vec3("offset", key.offset);
        return key;
    });
    /// @note 評価 (SampleFluidMotion) は昇順を前提に隣のキーと補間する。手で書いたファイルの並びを信用しない。
    std::stable_sort(motion.keys.begin(), motion.keys.end(),
                     [](const FluidMotionKey& a, const FluidMotionKey& b) { return a.time < b.time; });
}

toml::table WriteMotion(const FluidMotion& motion)
{
    Writer w;
    w.Bool("inherit_velocity", motion.inheritVelocity);
    WriteTableArray(w.table, "key", motion.keys, [](const FluidMotionKey& key) {
        Writer k;
        k.Float("time", key.time);
        k.Vec3("offset", key.offset);
        return std::move(k.table);
    });
    return std::move(w.table);
}

void ReadAmount(NodeView view, FluidAmount& amount)
{
    amount.keys.clear();
    ReadTableArray(view["key"], static_cast<std::size_t>(kMaxFluidAmountKeys), amount.keys, [](NodeView element) {
        FluidAmountKey key;
        const Reader k{ element };
        k.Float("time", key.time);
        k.Float("scale", key.scale);
        return key;
    });
    /// @note 評価 (SampleFluidAmount) は昇順を前提に隣のキーと補間する。手で書いたファイルの並びを信用しない。
    std::stable_sort(amount.keys.begin(), amount.keys.end(),
                     [](const FluidAmountKey& a, const FluidAmountKey& b) { return a.time < b.time; });
}

/// キーが 1 つも無ければ節ごと書かない (WriteTableArray が空の配列を書かないのと同じ理由に加えて、
/// «倍率 1» を意味する空の [source.amount] を既存の全ファイルへ増やさないため)。
void WriteAmountInto(toml::table& parent, const FluidAmount& amount)
{
    Writer w;
    WriteTableArray(w.table, "key", amount.keys, [](const FluidAmountKey& key) {
        Writer k;
        k.Float("time", key.time);
        k.Float("scale", key.scale);
        return std::move(k.table);
    });
    if (w.table.empty()) return;
    parent.insert("amount", std::move(w.table));
}

FluidSource ReadSource(NodeView view)
{
    FluidSource source;
    const Reader r{ view };
    r.Bool("enabled", source.enabled);
    r.Text("name", source.name);
    source.shape = FromLabel<FluidSourceShape>(
        view["shape"].value_or(std::string(LabelOf(source.shape, kShapeLabels))), kShapeLabels);
    r.Vec3("center", source.center);
    r.Vec3("size", source.size);
    r.Vec3("direction", source.direction);
    r.Text("texture", source.texture);
    r.Float("density", source.density);
    r.Float("temperature", source.temperature);
    r.Float("fuel", source.fuel);
    r.Float("noise", source.noise);
    r.Vec3("velocity", source.velocity);
    r.Float("start_time", source.startTime);
    r.Float("duration", source.duration);
    r.Float("color_key", source.colorKey);
    r.Float("spread", source.spread);
    r.Int("count", source.count);
    ReadMotion(view["motion"], source.motion);
    ReadAmount(view["amount"], source.amount);
    return source;
}

toml::table WriteSource(const FluidSource& source)
{
    Writer w;
    w.Bool("enabled", source.enabled);
    w.Text("name", source.name);
    w.Text("shape", LabelOf(source.shape, kShapeLabels));
    w.Vec3("center", source.center);
    w.Vec3("size", source.size);
    w.Vec3("direction", source.direction);
    w.Text("texture", source.texture);
    w.Float("density", source.density);
    w.Float("temperature", source.temperature);
    w.Float("fuel", source.fuel);
    w.Float("noise", source.noise);
    w.Vec3("velocity", source.velocity);
    w.Float("start_time", source.startTime);
    w.Float("duration", source.duration);
    w.Float("color_key", source.colorKey);
    w.Float("spread", source.spread);
    w.Int("count", source.count);
    w.table.insert("motion", WriteMotion(source.motion));
    WriteAmountInto(w.table, source.amount);
    return std::move(w.table);
}

FluidForce ReadForce(NodeView view)
{
    FluidForce force;
    const Reader r{ view };
    r.Bool("enabled", force.enabled);
    r.Text("name", force.name);
    force.type = FromLabel<FluidForceType>(
        view["type"].value_or(std::string(LabelOf(force.type, kForceTypeLabels))), kForceTypeLabels);
    r.Vec3("center", force.center);
    r.Vec3("direction", force.direction);
    r.Float("strength", force.strength);
    r.Float("radius", force.radius);
    r.Float("falloff_power", force.falloffPower);
    r.Float("noise_frequency", force.noiseFrequency);
    r.Float("noise_speed", force.noiseSpeed);
    r.Float("start_time", force.startTime);
    r.Float("duration", force.duration);
    ReadMotion(view["motion"], force.motion);
    ReadAmount(view["amount"], force.amount);
    return force;
}

toml::table WriteForce(const FluidForce& force)
{
    Writer w;
    w.Bool("enabled", force.enabled);
    w.Text("name", force.name);
    w.Text("type", LabelOf(force.type, kForceTypeLabels));
    w.Vec3("center", force.center);
    w.Vec3("direction", force.direction);
    w.Float("strength", force.strength);
    w.Float("radius", force.radius);
    w.Float("falloff_power", force.falloffPower);
    w.Float("noise_frequency", force.noiseFrequency);
    w.Float("noise_speed", force.noiseSpeed);
    w.Float("start_time", force.startTime);
    w.Float("duration", force.duration);
    w.table.insert("motion", WriteMotion(force.motion));
    WriteAmountInto(w.table, force.amount);
    return std::move(w.table);
}

FluidCollider ReadCollider(NodeView view)
{
    FluidCollider collider;
    const Reader r{ view };
    r.Bool("enabled", collider.enabled);
    r.Text("name", collider.name);
    collider.shape = FromLabel<FluidColliderShape>(
        view["shape"].value_or(std::string(LabelOf(collider.shape, kColliderShapeLabels))), kColliderShapeLabels);
    r.Vec3("center", collider.center);
    r.Vec3("size", collider.size);
    r.Vec3("direction", collider.direction);
    r.Float("friction", collider.friction);
    r.Float("start_time", collider.startTime);
    r.Float("duration", collider.duration);
    ReadMotion(view["motion"], collider.motion);
    return collider;
}

toml::table WriteCollider(const FluidCollider& collider)
{
    Writer w;
    w.Bool("enabled", collider.enabled);
    w.Text("name", collider.name);
    w.Text("shape", LabelOf(collider.shape, kColliderShapeLabels));
    w.Vec3("center", collider.center);
    w.Vec3("size", collider.size);
    w.Vec3("direction", collider.direction);
    w.Float("friction", collider.friction);
    w.Float("start_time", collider.startTime);
    w.Float("duration", collider.duration);
    w.table.insert("motion", WriteMotion(collider.motion));
    return std::move(w.table);
}

/// @name version 1 からの移行

FluidSource ReadV1GasSource(NodeView view)
{
    FluidSource source;
    const Reader r{ view };
    source.shape = FromLabel<FluidSourceShape>(
        view["shape"].value_or(std::string(LabelOf(source.shape, kShapeLabels))), kShapeLabels);
    r.Vec3("center", source.center);
    r.Vec3("size", source.size);
    r.Float("density", source.density);
    r.Float("temperature", source.temperature);
    r.Float("fuel", source.fuel);
    r.Vec3("velocity", source.velocity);
    r.Float("start_time", source.startTime);
    r.Float("duration", source.duration);
    r.Float("noise", source.noise);
    return source;
}

FluidSource ReadV1LiquidEmitter(NodeView view)
{
    /// @note version 1 の液体の発生源は既定値が今の FluidSource と違う。欠けたキーを今の既定で埋めると、
    ///       キーを省いて書いた古いファイルが «開いただけで別の絵» になる。当時の既定から読み始める。
    FluidSource source;
    source.shape = FluidSourceShape::Sphere;
    source.center = { 0.0f, -0.6f, 0.0f };
    source.velocity = { 0.0f, 2.5f, 0.0f };
    source.spread = 0.5f;
    source.count = 600;
    source.startTime = 0.0f;
    source.duration = 0.15f;
    float radius = 0.08f;
    const Reader r{ view };
    r.Vec3("position", source.center);
    r.Float("radius", radius);
    r.Vec3("velocity", source.velocity);
    r.Float("spread", source.spread);
    r.Int("count", source.count);
    r.Float("start_time", source.startTime);
    r.Float("duration", source.duration);
    source.size = { radius, radius, radius };
    return source;
}

void ReadRamp(NodeView view, FluidColorRamp& ramp)
{
    const toml::array* array = view.as_array();
    if (array == nullptr) return;
    const std::size_t count = (std::min)(array->size(), ramp.stops.size());
    for (std::size_t index = 0; index < count; ++index) {
        const toml::table* table = (*array)[index].as_table();
        if (table == nullptr) continue;
        const Reader r{ NodeView{ table } };
        r.Vec3("color", ramp.stops[index].color);
        r.Float("position", ramp.stops[index].position);
    }
    /// @note 評価は昇順を前提に隣の点と補間する (VolumeColorRamp と同じ規則)。
    std::stable_sort(ramp.stops.begin(), ramp.stops.end(),
                     [](const FluidColorStop& a, const FluidColorStop& b) { return a.position < b.position; });
}

toml::array WriteRamp(const FluidColorRamp& ramp)
{
    toml::array array;
    for (const FluidColorStop& stop : ramp.stops) {
        Writer w;
        w.Vec3("color", stop.color);
        w.Float("position", stop.position);
        array.push_back(std::move(w.table));
    }
    return array;
}

void ReadRender(NodeView view, FluidRenderSettings& look)
{
    const Reader r{ view };
    look.shading = ShadingFromName(view["shading"].value_or(std::string(ShadingName(look.shading))));
    r.Vec4("smoke_color", look.smokeColor);
    r.Vec4("shadow_color", look.shadowColor);
    r.Float("opacity", look.opacity);
    r.Float("self_shadow", look.selfShadow);
    r.Vec3("light_direction", look.lightDirection);
    r.Float("detail_strength", look.detailStrength);
    r.Float("detail_scale", look.detailScale);
    r.Float("fire_kelvin", look.fireKelvin);
    r.Float("fire_intensity", look.fireIntensity);
    r.Bool("use_emission_ramp", look.useEmissionRamp);
    ReadRamp(view["emission_ramp"], look.emissionRamp);
    r.Bool("use_albedo_ramp", look.useAlbedoRamp);
    ReadRamp(view["albedo_ramp"], look.albedoRamp);
    r.Vec4("liquid_color", look.liquidColor);
    r.Float("liquid_radius_scale", look.liquidRadiusScale);
    r.Float("liquid_threshold", look.liquidThreshold);
    r.Float("specular", look.specular);
    r.Float("liquid_softness", look.liquidSoftness);
    r.Float("liquid_extinction", look.liquidExtinction);
    r.Float("liquid_gloss", look.liquidGloss);
    r.Float("liquid_fresnel", look.liquidFresnel);
}

toml::table WriteRender(const FluidRenderSettings& look)
{
    Writer w;
    w.Text("shading", ShadingName(look.shading));
    w.Vec4("smoke_color", look.smokeColor);
    w.Vec4("shadow_color", look.shadowColor);
    w.Float("opacity", look.opacity);
    w.Float("self_shadow", look.selfShadow);
    w.Vec3("light_direction", look.lightDirection);
    w.Float("detail_strength", look.detailStrength);
    w.Float("detail_scale", look.detailScale);
    w.Float("fire_kelvin", look.fireKelvin);
    w.Float("fire_intensity", look.fireIntensity);
    w.Bool("use_emission_ramp", look.useEmissionRamp);
    w.table.insert("emission_ramp", WriteRamp(look.emissionRamp));
    w.Bool("use_albedo_ramp", look.useAlbedoRamp);
    w.table.insert("albedo_ramp", WriteRamp(look.albedoRamp));
    w.Vec4("liquid_color", look.liquidColor);
    w.Float("liquid_radius_scale", look.liquidRadiusScale);
    w.Float("liquid_threshold", look.liquidThreshold);
    w.Float("specular", look.specular);
    w.Float("liquid_softness", look.liquidSoftness);
    w.Float("liquid_extinction", look.liquidExtinction);
    w.Float("liquid_gloss", look.liquidGloss);
    w.Float("liquid_fresnel", look.liquidFresnel);
    return std::move(w.table);
}

void ReadOutput(NodeView view, FluidOutputSettings& output)
{
    const Reader r{ view };
    r.Int("frame_size", output.frameSize);
    r.Int("columns", output.columns);
    r.Int("rows", output.rows);
    r.Int("supersampling", output.supersampling);
    r.Float("duration", output.duration);
    r.Float("warmup", output.warmup);
    r.Int("substeps", output.substeps);
    r.Bool("loop", output.loop);
    r.Float("loop_blend_fraction", output.loopBlendFraction);
    r.Bool("motion_vectors", output.motionVectors);
    r.Bool("vector_field", output.vectorField);
    r.Int("vector_field_resolution", output.vectorFieldResolution);
    r.Vec3("vector_field_extents", output.vectorFieldExtents);
}

toml::table WriteOutput(const FluidOutputSettings& output)
{
    Writer w;
    w.Int("frame_size", output.frameSize);
    w.Int("columns", output.columns);
    w.Int("rows", output.rows);
    w.Int("supersampling", output.supersampling);
    w.Float("duration", output.duration);
    w.Float("warmup", output.warmup);
    w.Int("substeps", output.substeps);
    w.Bool("loop", output.loop);
    w.Float("loop_blend_fraction", output.loopBlendFraction);
    w.Bool("motion_vectors", output.motionVectors);
    w.Bool("vector_field", output.vectorField);
    w.Int("vector_field_resolution", output.vectorFieldResolution);
    w.Vec3("vector_field_extents", output.vectorFieldExtents);
    return std::move(w.table);
}

void ReadBake(NodeView view, FluidBakeSettings& bake)
{
    const Reader r{ view };
    bake.mode = BakeModeFromName(view["mode"].value_or(std::string(BakeModeName(bake.mode))));
    r.Int("volume_resolution", bake.volumeResolution);
    r.Int("ray_steps", bake.raySteps);
    r.Int("shadow_steps", bake.shadowSteps);
    bake.solver = BakeSolverFromName(view["solver"].value_or(std::string(BakeSolverName(bake.solver))));
    r.Float("density_scale", bake.densityScale);
    r.Int("scattering_octaves", bake.scatteringOctaves);
    r.Float("sky_occlusion", bake.skyOcclusion);
    r.Bool("blackbody_emission", bake.blackbodyEmission);
    r.Float("blackbody_min_kelvin", bake.blackbodyMinKelvin);
    r.Float("blackbody_max_kelvin", bake.blackbodyMaxKelvin);
    r.Bool("six_way_lightmaps", bake.sixWayLightmaps);
    r.Float("light_yaw_degrees", bake.lightYawDegrees);
    r.Float("light_pitch_degrees", bake.lightPitchDegrees);
    r.Vec3("light_color", bake.lightColor);
    r.Vec3("ambient", bake.ambient);
    r.Float("extinction", bake.extinction);
    r.Float("anisotropy", bake.anisotropy);
    r.Float("emission_intensity", bake.emissionIntensity);
    r.Float("exposure", bake.exposure);
    r.Float("camera_yaw_degrees", bake.cameraYawDegrees);
    r.Float("half_extent", bake.halfExtent);
}

toml::table WriteBake(const FluidBakeSettings& bake)
{
    Writer w;
    w.Text("mode", BakeModeName(bake.mode));
    w.Int("volume_resolution", bake.volumeResolution);
    w.Int("ray_steps", bake.raySteps);
    w.Int("shadow_steps", bake.shadowSteps);
    w.Text("solver", BakeSolverName(bake.solver));
    w.Float("density_scale", bake.densityScale);
    w.Int("scattering_octaves", bake.scatteringOctaves);
    w.Float("sky_occlusion", bake.skyOcclusion);
    w.Bool("blackbody_emission", bake.blackbodyEmission);
    w.Float("blackbody_min_kelvin", bake.blackbodyMinKelvin);
    w.Float("blackbody_max_kelvin", bake.blackbodyMaxKelvin);
    w.Bool("six_way_lightmaps", bake.sixWayLightmaps);
    w.Float("light_yaw_degrees", bake.lightYawDegrees);
    w.Float("light_pitch_degrees", bake.lightPitchDegrees);
    w.Vec3("light_color", bake.lightColor);
    w.Vec3("ambient", bake.ambient);
    w.Float("extinction", bake.extinction);
    w.Float("anisotropy", bake.anisotropy);
    w.Float("emission_intensity", bake.emissionIntensity);
    w.Float("exposure", bake.exposure);
    w.Float("camera_yaw_degrees", bake.cameraYawDegrees);
    w.Float("half_extent", bake.halfExtent);
    return std::move(w.table);
}

/// @name ReflectFluidRecipe
/// 範囲は FluidInspector / Volume Flipbook Bake パネルと各ベイカーの clamp に揃える。
/// JsonWriteReflector はこの範囲で丸めて書くので、AI が範囲外を送っても焼けない値にはならない。

/// 読むだけのリフレクタに通しても値を変えないよう、変わったときだけ書き戻す。
template <typename E, std::size_t N>
void ReflectEnum(scene::IReflector& r, const char* name, E& value, const char* const (&labels)[N])
{
    int index = static_cast<int>(value);
    r.Enum(name, index, std::span<const char* const>(labels));
    if (index != static_cast<int>(value)) value = static_cast<E>(std::clamp(index, 0, static_cast<int>(N) - 1));
}

/// FieldIf の範囲つき版。FieldIf は Field へ流すので、JSON 側の範囲の丸めが効かなくなる。
void FloatRangeIf(scene::IReflector& r, const char* name, float& value, float min, float max, bool visible,
                  const char* tooltip = nullptr)
{
    r.BeginField(name, name);
    r.SetFieldVisible(visible);
    r.FloatRange(name, value, min, max);
    if (visible && tooltip != nullptr) r.Tooltip(tooltip);
    r.EndField();
}

void IntRangeIf(scene::IReflector& r, const char* name, int& value, int min, int max, bool visible,
                const char* tooltip = nullptr)
{
    r.BeginField(name, name);
    r.SetFieldVisible(visible);
    r.IntRange(name, value, min, max);
    if (visible && tooltip != nullptr) r.Tooltip(tooltip);
    r.EndField();
}

/// @brief 要素数は maxCount で切る (Inspector の Add・AI の長い配列のどちらも)。
/// @note 部品の数は GPU の定数バッファに載る数で決まっており、CPU だけ多く置けると焼き分けで絵が変わる。
template <typename T, typename ReflectElement>
void ReflectList(scene::IReflector& r, const char* name, std::vector<T>& items, std::size_t maxCount,
                 ReflectElement reflectElement)
{
    const std::size_t count = (std::min)(r.BeginObjectList(name, items.size()), maxCount);
    items.resize(count);
    for (std::size_t index = 0; index < items.size(); ++index) {
        r.BeginObjectElement(index);
        reflectElement(items[index]);
        r.EndObjectElement();
    }
    const std::size_t removeIndex = r.EndObjectList();
    if (removeIndex < items.size()) {
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        return;
    }
    std::size_t from = 0;
    std::size_t to = 0;
    if (r.ObjectListMove(from, to) && from < items.size() && to < items.size() && from != to) {
        T moved = std::move(items[from]);
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    }
}

void ReflectGas(FluidGasSettings& gas, scene::IReflector& r)
{
    r.IntRange("resolution", gas.resolution, 0, 512);
    r.Tooltip("2D ベイクの格子の 1 辺。0 は Auto (コマの解像度に合わせる。上限 256)");
    r.FloatRange("buoyancy", gas.buoyancy, -10.0f, 20.0f);
    r.FloatRange("weight", gas.weight, 0.0f, 10.0f);
    r.FloatRange("vorticity", gas.vorticity, 0.0f, 4.0f);
    r.FloatRange("turbulence", gas.turbulence, 0.0f, 10.0f);
    r.FloatRange("turbulence_scale", gas.turbulenceScale, 0.1f, 32.0f);
    r.FloatRange("density_dissipation", gas.densityDissipation, 0.0f, 10.0f);
    r.FloatRange("temperature_dissipation", gas.temperatureDissipation, 0.0f, 20.0f);
    r.FloatRange("velocity_damping", gas.velocityDamping, 0.0f, 10.0f);
    r.FloatRange("ignition_temperature", gas.ignitionTemperature, 0.0f, 10.0f);
    r.FloatRange("burn_rate", gas.burnRate, 0.0f, 50.0f);
    r.FloatRange("burn_heat", gas.burnHeat, 0.0f, 20.0f);
    r.FloatRange("burn_smoke", gas.burnSmoke, 0.0f, 10.0f);
    r.FloatRange("burn_expansion", gas.burnExpansion, 0.0f, 20.0f);
    r.Field("wind", gas.wind);
    r.Field("floor", gas.floor);
    r.IntRange("pressure_iterations", gas.pressureIterations, 8, 200);
    r.Field("sharp_advection", gas.sharpAdvection);
    r.FloatRange("detail_period", gas.detailPeriod, 0.05f, 10.0f);
}

void ReflectLiquid(FluidLiquidSettings& liquid, scene::IReflector& r)
{
    r.IntRange("max_particles", liquid.maxParticles, 100, 20000);
    r.FloatRange("particle_radius", liquid.particleRadius, 0.004f, 0.05f);
    r.FloatRange("gravity", liquid.gravity, -20.0f, 40.0f);
    r.FloatRange("viscosity", liquid.viscosity, 0.0f, 1.0f);
    r.FloatRange("cohesion", liquid.cohesion, 0.0f, 1.0f);
    r.IntRange("solver_iterations", liquid.solverIterations, 1, 10);
    r.Field("floor", liquid.floor);
    r.FloatRange("floor_height", liquid.floorHeight, -1.5f, 1.0f);
    r.FloatRange("floor_friction", liquid.floorFriction, 0.0f, 2.0f);
    r.FloatRange("particle_lifetime", liquid.particleLifetime, 0.0f, 30.0f);
    r.Tooltip("0 は無限");
}

void ReflectMotion(FluidMotion& motion, scene::IReflector& r)
{
    r.BeginObject("motion");
    r.Field("inherit_velocity", motion.inheritVelocity);
    r.Tooltip("動く速さを流速 (気体) / 撃ち出す速度 (液体) に足す");
    ReflectList(r, "key", motion.keys, static_cast<std::size_t>(kMaxFluidMotionKeys), [&r](FluidMotionKey& key) {
        r.FloatRange("time", key.time, 0.0f, 60.0f);
        r.Tooltip("キーの間は直線でつなぐ。先頭より前・末尾より後は端のキーで止まる");
        r.Field("offset", key.offset);
        r.Tooltip("center からのずれ (正規化単位)");
    });
    r.EndObject();
}

void ReflectAmount(FluidAmount& amount, const char* scaleTooltip, scene::IReflector& r)
{
    r.BeginObject("amount");
    ReflectList(r, "key", amount.keys, static_cast<std::size_t>(kMaxFluidAmountKeys), [&](FluidAmountKey& key) {
        r.FloatRange("time", key.time, 0.0f, 60.0f);
        r.Tooltip("キーの間は直線でつなぐ。先頭より前・末尾より後は端のキーで止まる");
        r.FloatRange("scale", key.scale, 0.0f, 8.0f);
        r.Tooltip(scaleTooltip);
    });
    r.EndObject();
}

void ReflectSource(FluidSource& source, bool liquid, scene::IReflector& r)
{
    r.Field("enabled", source.enabled);
    r.Field("name", source.name);
    ReflectEnum(r, "shape", source.shape, kShapeLabels);
    r.Tooltip("sphere: size.x = 半径 / box: size = 各軸の半分 / cone: 頂点が center、size.x = 底の半径、"
              "size.y = 長さ / ring: size.x = 輪の半径、size.y = 管の太さ / texture: 画像の濃さ (輝度 × α) の形に湧く板 / "
              "capsule: size.x = 半径、size.y = 芯の半分の長さ (両端は半球) / cylinder: size.x = 半径、size.y = 半分の高さ");
    r.Field("center", source.center);
    r.Tooltip("領域を各軸 [-1,1] に正規化した座標");
    r.Field("size", source.size);
    const bool textured = source.shape == FluidSourceShape::Texture;
    const bool segment =
        source.shape == FluidSourceShape::Capsule || source.shape == FluidSourceShape::Cylinder;
    if (textured) r.Tooltip("size.x / size.y = 板の半幅 / 半高さ、size.z = 板の厚みの半分");
    else if (segment) r.Tooltip("size.x = 半径、size.y = 軸方向の長さの半分 (size.z は見ない)");
    const bool oriented = source.shape == FluidSourceShape::Cone || source.shape == FluidSourceShape::Ring
        || textured || segment;
    r.FieldIf("direction", source.direction, oriented,
              "cone の開く向き / ring・texture の法線 / capsule・cylinder の軸 (長さは問わない)。"
              "texture は (0,0,1) で画像が正面を向く");
    r.BeginField("texture", "texture");
    r.SetFieldVisible(textured);
    r.SetFileExtensions(".sprite,.png,.tga,.jpg,.jpeg");
    r.Field("texture", source.texture);
    if (textured) r.Tooltip("256×256 に縮めて使う。縦横比は size.x / size.y が持つ。"
                            "Sprite ならそのコマだけを切り抜く");
    r.EndField();
    FloatRangeIf(r, "density", source.density, 0.0f, 100.0f, !liquid, "1 秒あたりに足す量");
    FloatRangeIf(r, "temperature", source.temperature, 0.0f, 100.0f, !liquid);
    FloatRangeIf(r, "fuel", source.fuel, 0.0f, 100.0f, !liquid);
    FloatRangeIf(r, "noise", source.noise, 0.0f, 1.0f, !liquid, "注入量をノイズで揺らす");
    r.Field("velocity", source.velocity);
    r.Tooltip(liquid ? "撃ち出す速度 [領域単位/秒]" : "発生源の中の流速。0 なら流速には触らない");
    r.FloatRange("start_time", source.startTime, 0.0f, 60.0f);
    r.FloatRange("duration", source.duration, 0.0f, 60.0f);
    r.Tooltip(liquid ? "0 なら start_time に一斉に出す" : "0 は最後まで出し続ける");
    r.FloatRange("color_key", source.colorKey, 0.0f, 1.0f);
    r.Tooltip(liquid ? "render.albedo_ramp のどの色で描くか (粒子ごとに持つ)。use_albedo_ramp が false なら効かない"
                     : "render.albedo_ramp のどの色で描くか (煙に乗って運ばれ、混ざると色も混ざる)。"
                       "use_albedo_ramp が false なら効かない");
    FloatRangeIf(r, "spread", source.spread, 0.0f, 1.0f, liquid, "撃ち出す速度の大きさに対するばらつき");
    IntRangeIf(r, "count", source.count, 1, 20000, liquid, "撃ち出す総数");
    ReflectMotion(source.motion, r);
    ReflectAmount(source.amount,
                  liquid ? "density / temperature / fuel に掛かる倍率。キーが無ければ 1 "
                           "(液体はこの 3 つを使わないので効かない — 撒く量は count と duration が決める)"
                         : "density / temperature / fuel に掛かる倍率。キーが無ければ 1。"
                           "位置と撃ち出す速度には掛からない (動きは motion が持つ)",
                  r);
}

void ReflectForce(FluidForce& force, scene::IReflector& r)
{
    r.Field("enabled", force.enabled);
    r.Field("name", force.name);
    ReflectEnum(r, "type", force.type, kForceTypeLabels);
    r.Field("center", force.center);
    r.Tooltip("領域を各軸 [-1,1] に正規化した座標");
    const bool directed = force.type == FluidForceType::Wind || force.type == FluidForceType::Vortex;
    r.FieldIf("direction", force.direction, directed, "wind: 向き / vortex: 回転軸 (2D では常に画面の奥行き軸)");
    /// @note 負を許すのは 2D の vortex の回り方を逆にする手段がこれしか無いため (軸は奥行きに固定)。
    r.FloatRange("strength", force.strength, -50.0f, 50.0f);
    r.Tooltip("加速度 [領域単位/秒²]。負で逆向き。drag は減衰係数 [1/秒] (0 以上)");
    r.FloatRange("radius", force.radius, 0.0f, 4.0f);
    r.Tooltip("影響半径。0 は領域全体に一様");
    FloatRangeIf(r, "falloff_power", force.falloffPower, 0.1f, 8.0f, force.radius > 0.0f,
                 "influence = (1 - 距離/radius)^falloff_power");
    const bool noise = force.type == FluidForceType::Noise;
    FloatRangeIf(r, "noise_frequency", force.noiseFrequency, 0.1f, 32.0f, noise, "領域幅あたりの山の数");
    FloatRangeIf(r, "noise_speed", force.noiseSpeed, 0.0f, 10.0f, noise, "ノイズが流れる速さ");
    r.FloatRange("start_time", force.startTime, 0.0f, 60.0f);
    r.FloatRange("duration", force.duration, 0.0f, 60.0f);
    r.Tooltip("0 はずっと効く");
    ReflectMotion(force.motion, r);
    ReflectAmount(force.amount, "strength に掛かる倍率。キーが無ければ 1", r);
}

void ReflectCollider(FluidCollider& collider, bool liquid, scene::IReflector& r)
{
    r.Field("enabled", collider.enabled);
    r.Field("name", collider.name);
    ReflectEnum(r, "shape", collider.shape, kColliderShapeLabels);
    r.Tooltip("sphere: size.x = 半径 / box: size = 各軸の半分 (回転なし) / "
              "plane: center を通り direction を法線とする面。裏側がすべて固体 / "
              "capsule: size.x = 半径、size.y = 芯の半分の長さ (両端は半球) / cylinder: size.x = 半径、size.y = 半分の高さ");
    r.Field("center", collider.center);
    r.Tooltip("領域を各軸 [-1,1] に正規化した座標");
    const bool plane = collider.shape == FluidColliderShape::Plane;
    const bool segment =
        collider.shape == FluidColliderShape::Capsule || collider.shape == FluidColliderShape::Cylinder;
    r.FieldIf("size", collider.size, !plane,
              segment ? "size.x = 半径、size.y = 軸方向の長さの半分 (size.z は見ない)"
                      : "sphere: size.x = 半径 / box: 各軸の半分");
    r.FieldIf("direction", collider.direction, plane || segment,
              segment ? "軸の向き。長さは問わない (0 なら上向き)" : "面の法線。こちら側が流体 (長さは問わない)");
    FloatRangeIf(r, "friction", collider.friction, 0.0f, 2.0f, liquid, "表面に沿った速度を落とす強さ");
    r.FloatRange("start_time", collider.startTime, 0.0f, 60.0f);
    r.FloatRange("duration", collider.duration, 0.0f, 60.0f);
    r.Tooltip("0 はずっと居る");
    ReflectMotion(collider.motion, r);
}

void ReflectRamp(const char* name, FluidColorRamp& ramp, bool visible, const char* positionTooltip,
                 scene::IReflector& r)
{
    r.BeginField(name, name);
    r.SetFieldVisible(visible);
    /// @note 点の数は GPU / 3D の Ramp と同じ 4 で固定。足し引きさせない。
    r.SetFixedList(true);
    const std::size_t count = (std::min)(r.BeginObjectList(name, ramp.stops.size()), ramp.stops.size());
    /// @note 保存キーは «次の BeginField まで» 残る。戻さないと要素の color / position が Ramp の名で記録される。
    r.EndField();
    for (std::size_t index = 0; index < count; ++index) {
        FluidColorStop& stop = ramp.stops[index];
        r.BeginObjectElement(index);
        /// @note ColorField にしないのは、カラーピッカーが 1 を超える値 (HDR の芯) を丸めてしまうため。
        r.Field("color", stop.color);
        r.Tooltip("リニア (HDR 可)");
        r.FloatRange("position", stop.position, 0.0f, 1.0f);
        r.Tooltip(positionTooltip);
        r.EndObjectElement();
    }
    (void)r.EndObjectList();
}

void ReflectRender(FluidRenderSettings& look, bool liquid, scene::IReflector& r)
{
    ReflectEnum(r, "shading", look.shading, kShadingLabels);
    r.ColorField("smoke_color", look.smokeColor);
    r.ColorField("shadow_color", look.shadowColor);
    r.FloatRange("opacity", look.opacity, 0.0f, 50.0f);
    r.FloatRange("self_shadow", look.selfShadow, 0.0f, 20.0f);
    r.Field("light_direction", look.lightDirection);
    r.FloatRange("detail_strength", look.detailStrength, 0.0f, 1.0f);
    r.FloatRange("detail_scale", look.detailScale, 1.0f, 64.0f);
    r.FloatRange("fire_kelvin", look.fireKelvin, 500.0f, 6000.0f);
    r.FloatRange("fire_intensity", look.fireIntensity, 0.0f, 10.0f);
    r.Field("use_emission_ramp", look.useEmissionRamp);
    r.Tooltip("温度 (0〜1) → 発光の色を emission_ramp で決める。false なら fire は黒体、glow は smoke_color");
    ReflectRamp("emission_ramp", look.emissionRamp, look.useEmissionRamp,
                "温度 (0〜1) のどこに置くか。昇順に並べる", r);
    r.Field("use_albedo_ramp", look.useAlbedoRamp);
    r.Tooltip("発生源の color_key (0〜1) → 煙 / 液の地の色を albedo_ramp で決める。"
              "false なら smoke_color (glow では発光色) / liquid_color の 1 色");
    ReflectRamp("albedo_ramp", look.albedoRamp, look.useAlbedoRamp,
                "color_key (0〜1) のどこに置くか。昇順に並べる", r);
    r.ColorField("liquid_color", look.liquidColor);
    r.FloatRange("liquid_radius_scale", look.liquidRadiusScale, 0.5f, 6.0f);
    r.FloatRange("liquid_threshold", look.liquidThreshold, 0.05f, 2.0f);
    r.FloatRange("specular", look.specular, 0.0f, 2.0f);
    FloatRangeIf(r, "liquid_softness", look.liquidSoftness, 0.005f, 0.5f, liquid, "3D の液面: 縁の柔らかさ (密度の幅)");
    FloatRangeIf(r, "liquid_extinction", look.liquidExtinction, 0.0f, 200.0f, liquid,
                 "3D の液面: 濃さ。大きいほど不透明 (血)、小さいほど透ける (水)");
    FloatRangeIf(r, "liquid_gloss", look.liquidGloss, 1.0f, 512.0f, liquid, "3D の液面: 鏡面反射の鋭さ");
    FloatRangeIf(r, "liquid_fresnel", look.liquidFresnel, 0.0f, 0.2f, liquid, "3D の液面: 正面から見た反射率 (水 0.02)");
}

void ReflectOutput(FluidOutputSettings& output, scene::IReflector& r)
{
    r.IntRange("frame_size", output.frameSize, 16, 1024);
    r.IntRange("columns", output.columns, 1, 32);
    r.IntRange("rows", output.rows, 1, 32);
    r.IntRange("supersampling", output.supersampling, 1, 4);
    r.FloatRange("duration", output.duration, 0.05f, 30.0f);
    r.FloatRange("warmup", output.warmup, 0.0f, 30.0f);
    r.IntRange("substeps", output.substeps, 1, 16);
    r.Field("loop", output.loop);
    r.FloatRange("loop_blend_fraction", output.loopBlendFraction, 0.0f, 0.5f);
    r.Field("motion_vectors", output.motionVectors);
    r.Field("vector_field", output.vectorField);
    r.IntRange("vector_field_resolution", output.vectorFieldResolution, 8, 64);
    r.Field("vector_field_extents", output.vectorFieldExtents);
}

void ReflectBake(FluidBakeSettings& bake, scene::IReflector& r)
{
    ReflectEnum(r, "mode", bake.mode, kBakeModeLabels);
    r.Tooltip("2d は .fluid の Bake (2D ソルバー)、3d は Volume Flipbook Baker (3D で解いてレイマーチ)");
    r.IntRange("volume_resolution", bake.volumeResolution, 16, 160);
    r.Tooltip("3D の格子の 1 辺。GPU は 160、CPU は 96 まで");
    r.IntRange("ray_steps", bake.raySteps, 32, 512);
    r.Tooltip("視線 1 本の標本数。volume_resolution の 2 倍が目安 (据え置きだと格子を上げても絵が変わらない)");
    r.IntRange("shadow_steps", bake.shadowSteps, 4, 64);
    r.Tooltip("影の行進の標本数。増やすと影の縞が減るが、焼き時間は ray_steps との積で増える");
    ReflectEnum(r, "solver", bake.solver, kBakeSolverLabels);
    r.Tooltip("3D で解くソルバー。液体の gpu は GPU の粒子ソルバー (新しく検証中。液体のプリセットは cpu)");
    r.FloatRange("density_scale", bake.densityScale, 0.01f, 20.0f);
    r.IntRange("scattering_octaves", bake.scatteringOctaves, 1, 8);
    r.FloatRange("sky_occlusion", bake.skyOcclusion, 0.0f, 1.0f);
    r.Field("blackbody_emission", bake.blackbodyEmission);
    r.FloatRange("blackbody_min_kelvin", bake.blackbodyMinKelvin, 500.0f, 15000.0f);
    r.FloatRange("blackbody_max_kelvin", bake.blackbodyMaxKelvin, 500.0f, 15000.0f);
    r.Field("six_way_lightmaps", bake.sixWayLightmaps);
    r.FloatRange("light_yaw_degrees", bake.lightYawDegrees, -180.0f, 180.0f);
    r.FloatRange("light_pitch_degrees", bake.lightPitchDegrees, -10.0f, 90.0f);
    r.ColorField("light_color", bake.lightColor);
    r.ColorField("ambient", bake.ambient);
    r.FloatRange("extinction", bake.extinction, 0.0f, 100.0f);
    r.FloatRange("anisotropy", bake.anisotropy, -0.95f, 0.95f);
    r.FloatRange("emission_intensity", bake.emissionIntensity, 0.0f, 100.0f);
    r.FloatRange("exposure", bake.exposure, 0.01f, 8.0f);
    r.FloatRange("camera_yaw_degrees", bake.cameraYawDegrees, -180.0f, 180.0f);
    r.FloatRange("half_extent", bake.halfExtent, 0.05f, 4.0f);
}

} /// @note namespace

const char* FluidPresetName(FluidPreset preset)
{
    switch (preset) {
    case FluidPreset::Smoke:       return "Smoke Puff";
    case FluidPreset::Fire:        return "Fire (loop)";
    case FluidPreset::Explosion:   return "Explosion";
    case FluidPreset::Steam:       return "Steam (loop)";
    case FluidPreset::DustBurst:   return "Dust Burst";
    case FluidPreset::Ink:         return "Ink Swirl";
    case FluidPreset::MagicWisp:   return "Magic Wisp (loop)";
    case FluidPreset::HeatHaze:    return "Heat Haze (loop)";
    case FluidPreset::WaterSplash: return "Water Splash";
    case FluidPreset::WaterJet:    return "Water Jet (loop)";
    case FluidPreset::BloodBurst:  return "Blood Burst";
    case FluidPreset::LavaBlob:    return "Lava Blob";
    case FluidPreset::PlasmaBurst: return "Plasma Burst";
    case FluidPreset::ArcHaze:     return "Arc Haze (loop)";
    case FluidPreset::GroundRing:  return "Ground Shock Ring";
    case FluidPreset::ColdMist:    return "Cold Mist";
    case FluidPreset::ChargeVortex:return "Charge Vortex";
    case FluidPreset::EmberBurst:  return "Ember Burst";
    case FluidPreset::SigilFlare:  return "Sigil Flare";
    case FluidPreset::Count:
    default:                       return "Unknown";
    }
}

FluidRecipe MakeFluidPreset(FluidPreset preset)
{
    FluidRecipe recipe;
    recipe.seed = 1u + static_cast<uint32_t>(preset);
    FluidGasSettings& gas = recipe.gas;
    FluidLiquidSettings& liquid = recipe.liquid;
    FluidRenderSettings& look = recipe.render;
    FluidOutputSettings& output = recipe.output;
    /// @note 返す参照は次の push_back で無効になる。1 つの発生源を設定し終えてから次を足すこと。
    const auto addSource = [&recipe](const char* name, const math::Vector3& center, float radius) -> FluidSource& {
        FluidSource source;
        source.name = name;
        source.center = center;
        source.size = { radius, radius, radius };
        recipe.sources.push_back(std::move(source));
        return recipe.sources.back();
    };
    const auto addLiquidSource = [&recipe](const char* name, const math::Vector3& center, float radius)
        -> FluidSource& {
        recipe.kind = FluidKind::Liquid;
        recipe.render.shading = FluidShading::Liquid;
        /// @note 液面の縁は 1px の閾値で決まるのでギザギザが目立つ。気体より超解像が効く。
        recipe.output.supersampling = 2;
        FluidSource source;
        source.name = name;
        source.shape = FluidSourceShape::Sphere;
        source.center = center;
        source.size = { radius, radius, radius };
        recipe.sources.push_back(std::move(source));
        return recipe.sources.back();
    };
    const auto addRing = [&recipe](const char* name, const math::Vector3& center, float radius, float tube)
        -> FluidSource& {
        FluidSource source;
        source.name = name;
        source.shape = FluidSourceShape::Ring;
        source.center = center;
        source.size = { radius, tube, tube };
        source.direction = { 0.0f, 1.0f, 0.0f };
        recipe.sources.push_back(std::move(source));
        return recipe.sources.back();
    };
    const auto addCone = [&recipe](const char* name, const math::Vector3& center, const math::Vector3& direction,
                                   float radius, float length) -> FluidSource& {
        FluidSource source;
        source.name = name;
        source.shape = FluidSourceShape::Cone;
        source.center = center;
        source.size = { radius, length, radius };
        source.direction = direction;
        recipe.sources.push_back(std::move(source));
        return recipe.sources.back();
    };
    const auto addForce = [&recipe](const char* name, FluidForceType type, float strength) -> FluidForce& {
        FluidForce force;
        force.name = name;
        force.type = type;
        force.strength = strength;
        recipe.forces.push_back(std::move(force));
        return recipe.forces.back();
    };

    switch (preset) {
    case FluidPreset::Smoke: {
        /// @note 芯・外周リング・左右の耳の 3 役で湧かせ、repulse で押し開いてタイルを埋めるまで育てる。
        ///       1 つの球だけだと «小さな煙玉が中央でくすぶる» 絵にしかならない。
        FluidSource& core = addSource("Core", { 0.0f, -0.5f, 0.0f }, 0.26f);
        core.density = 6.0f;
        core.temperature = 3.0f;
        core.velocity = { 0.0f, 0.8f, 0.0f };
        core.duration = 0.5f;
        core.noise = 0.6f;
        FluidSource& collar = addRing("Collar", { 0.0f, -0.48f, 0.0f }, 0.32f, 0.11f);
        collar.density = 3.5f;
        collar.temperature = 1.2f;
        collar.duration = 0.45f;
        collar.noise = 0.8f;
        collar.colorKey = 1.0f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& wing = addSource(side == 0 ? "Wing L" : "Wing R", { sign * 0.38f, -0.58f, 0.0f }, 0.14f);
            wing.density = 2.5f;
            wing.temperature = 0.8f;
            wing.velocity = { sign * 0.7f, 0.7f, 0.0f };
            wing.duration = 0.4f;
            wing.noise = 0.8f;
            wing.colorKey = 0.6f;
        }
        FluidForce& bloom = addForce("Bloom", FluidForceType::Repulse, 1.2f);
        bloom.center = { 0.0f, -0.45f, 0.0f };
        bloom.radius = 0.8f;
        bloom.falloffPower = 1.3f;
        bloom.duration = 0.45f;
        FluidForce& roll = addForce("Roll", FluidForceType::Vortex, 0.8f);
        roll.center = { 0.0f, 0.05f, 0.0f };
        roll.direction = { 0.0f, 0.0f, 1.0f };
        roll.radius = 0.9f;
        roll.falloffPower = 1.5f;
        FluidForce& breakUp = addForce("Break Up", FluidForceType::Noise, 1.2f);
        breakUp.noiseFrequency = 2.5f;
        breakUp.noiseSpeed = 0.8f;
        gas.buoyancy = 1.1f;
        gas.weight = 0.02f;
        gas.vorticity = 0.55f;
        gas.turbulence = 0.35f;
        gas.turbulenceScale = 3.5f;
        gas.densityDissipation = 0.14f;
        gas.temperatureDissipation = 0.9f;
        gas.velocityDamping = 0.12f;
        look.shading = FluidShading::Smoke;
        look.useAlbedoRamp = true;
        /// @note 鍵 0 = 芯の濃い灰、鍵 1 = 外周の明るい灰。混ざる境目が «厚み» に見える。
        look.albedoRamp = { { FluidColorStop{ { 0.16f, 0.16f, 0.17f }, 0.0f },
                              FluidColorStop{ { 0.24f, 0.24f, 0.25f }, 0.33f },
                              FluidColorStop{ { 0.36f, 0.36f, 0.38f }, 0.66f },
                              FluidColorStop{ { 0.50f, 0.50f, 0.53f }, 1.0f } } };
        output.duration = 2.5f;
        output.substeps = 3;
        break;
    }
    case FluidPreset::Fire: {
        /// @note 燃料だけを流し込み、燃えた所に熱と少しの煤が生まれる。炎の長さは Cooling で決まる。
        ///       口を 1 つにすると «細い蝋燭» になる。中央の柱に左右の舌と根元の輪を足して幅を作り、
        ///       逆向きの渦 2 つで舌を内側へ巻き込む。煙は注がない (煤は燃焼からしか生まれない)。
        FluidSource& column = addCone("Fuel Column", { 0.0f, -0.95f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.24f, 0.6f);
        column.density = 0.0f;
        column.fuel = 6.5f;
        column.temperature = 2.2f;
        column.velocity = { 0.0f, 1.6f, 0.0f };
        column.noise = 0.9f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& flare = addCone(side == 0 ? "Flare L" : "Flare R", { sign * 0.34f, -0.9f, 0.0f },
                                         { sign * 0.3f, 1.0f, 0.0f }, 0.13f, 0.42f);
            flare.density = 0.0f;
            flare.fuel = 3.0f;
            flare.temperature = 1.6f;
            flare.velocity = { sign * 0.35f, 1.4f, 0.0f };
            flare.noise = 1.0f;
        }
        FluidSource& embers = addRing("Ember Ring", { 0.0f, -0.82f, 0.0f }, 0.42f, 0.08f);
        embers.density = 0.0f;
        embers.fuel = 1.8f;
        embers.temperature = 1.4f;
        embers.noise = 1.0f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidForce& curl = addForce(side == 0 ? "Curl L" : "Curl R", FluidForceType::Vortex, sign * -1.1f);
            curl.center = { sign * 0.3f, -0.25f, 0.0f };
            curl.direction = { 0.0f, 0.0f, 1.0f };
            curl.radius = 0.55f;
        }
        FluidForce& flicker = addForce("Flicker", FluidForceType::Noise, 1.4f);
        flicker.noiseFrequency = 4.0f;
        flicker.noiseSpeed = 1.5f;
        gas.buoyancy = 1.8f;
        gas.weight = 0.0f;
        gas.vorticity = 0.6f;
        gas.turbulence = 0.45f;
        gas.turbulenceScale = 4.0f;
        gas.densityDissipation = 0.5f;
        gas.temperatureDissipation = 1.8f;
        gas.ignitionTemperature = 0.2f;
        gas.burnRate = 6.0f;
        gas.burnHeat = 1.3f;
        gas.burnSmoke = 0.6f;
        gas.burnExpansion = 0.6f;
        look.shading = FluidShading::Fire;
        look.smokeColor = { 0.25f, 0.23f, 0.22f, 1.0f };
        look.shadowColor = { 0.05f, 0.04f, 0.04f, 1.0f };
        look.opacity = 2.6f;
        look.selfShadow = 1.5f;
        look.fireKelvin = 1400.0f;
        output.duration = 1.5f;
        output.warmup = 1.0f;
        output.substeps = 4;
        output.loop = true;
        /// @note 3D の黒体は温度 1 を blackbodyMaxKelvin に当てる。芯だけが白く、先端が暗い赤に落ちる幅にする。
        recipe.bake.blackbodyEmission = true;
        recipe.bake.blackbodyMinKelvin = 900.0f;
        recipe.bake.blackbodyMaxKelvin = 2200.0f;
        break;
    }
    case FluidPreset::Explosion: {
        /// @note 一瞬で大量の燃料を燃やす。膨張が圧力解法を押し広げて火球と爆風になる。
        FluidSource& source = addSource("Core", { 0.0f, -0.1f, 0.0f }, 0.2f);
        source.density = 0.8f;
        source.fuel = 10.0f;
        source.temperature = 2.6f;
        source.duration = 0.14f;
        source.noise = 0.7f;
        /// @note 火球の周りに明るい土煙の輪を置く (2D の断面では芯の左右 2 か所)。芯の煤と色の鍵を分け、
        ///       黒い芯と灰茶の外側が混ざる境目を albedo_ramp で描く (発生源ごとの色の見本)。
        ///       燃料は持たせない: 燃えると膨張で芯より先に爆ぜ、火球の形が崩れる。
        FluidSource& dust = addRing("Dust Ring", { 0.0f, -0.14f, 0.0f }, 0.38f, 0.11f);
        dust.density = 3.2f;
        dust.temperature = 0.4f;
        dust.duration = 0.25f;
        dust.noise = 0.85f;
        dust.colorKey = 1.0f;
        /// @note 火球より一回り外を遅れて走る «衝撃の輪» と、そこから千切れて飛ぶ塊。芯の煙が届く前に
        ///       画面の端まで «何かが来た» と分かる。
        FluidSource& shock = addRing("Shock Ring", { 0.0f, -0.1f, 0.0f }, 0.6f, 0.08f);
        shock.density = 1.6f;
        shock.temperature = 0.2f;
        shock.startTime = 0.12f;
        shock.duration = 0.28f;
        shock.noise = 0.9f;
        shock.colorKey = 0.66f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& debris =
                addSource(side == 0 ? "Debris L" : "Debris R", { sign * 0.52f, -0.18f, 0.0f }, 0.12f);
            debris.density = 1.6f;
            debris.temperature = 0.3f;
            debris.velocity = { sign * 1.2f, 0.5f, 0.0f };
            debris.duration = 0.2f;
            debris.noise = 0.9f;
            debris.colorKey = 0.33f;
        }
        FluidForce& blast = addForce("Blast", FluidForceType::Repulse, 2.6f);
        blast.center = { 0.0f, -0.1f, 0.0f };
        blast.radius = 0.9f;
        blast.falloffPower = 1.2f;
        blast.duration = 0.3f;
        FluidForce& churn = addForce("Churn", FluidForceType::Vortex, 1.2f);
        churn.direction = { 0.0f, 0.0f, 1.0f };
        churn.radius = 1.0f;
        FluidForce& shred = addForce("Shred", FluidForceType::Noise, 1.5f);
        shred.noiseFrequency = 3.5f;
        shred.noiseSpeed = 1.2f;
        look.useAlbedoRamp = true;
        /// @note リニア。鍵 0 = 芯の煤 (smoke_color より一段暗い)、鍵 1 = 外側の土煙。
        look.albedoRamp = { { FluidColorStop{ { 0.050f, 0.045f, 0.040f }, 0.0f },
                              FluidColorStop{ { 0.125f, 0.105f, 0.087f }, 0.33f },
                              FluidColorStop{ { 0.200f, 0.165f, 0.133f }, 0.66f },
                              FluidColorStop{ { 0.280f, 0.230f, 0.180f }, 1.0f } } };
        gas.buoyancy = 1.2f;
        gas.weight = 0.02f;
        gas.vorticity = 0.7f;
        gas.turbulence = 0.35f;
        gas.densityDissipation = 0.25f;
        gas.temperatureDissipation = 1.5f;
        gas.ignitionTemperature = 0.2f;
        gas.burnRate = 9.0f;
        gas.burnHeat = 1.8f;
        gas.burnSmoke = 0.9f;
        gas.burnExpansion = 3.0f;
        look.shading = FluidShading::Fire;
        look.smokeColor = { 0.32f, 0.29f, 0.27f, 1.0f };
        look.shadowColor = { 0.06f, 0.05f, 0.05f, 1.0f };
        look.opacity = 5.0f;
        look.fireKelvin = 1700.0f;
        output.duration = 2.2f;
        output.substeps = 4;
        recipe.bake.blackbodyEmission = true;
        recipe.bake.blackbodyMinKelvin = 1000.0f;
        recipe.bake.blackbodyMaxKelvin = 2600.0f;
        break;
    }
    case FluidPreset::Steam: {
        /// @note 細い口から噴き上がる蒸気。円錐は口で細く、上へ開くので «噴く» 形が球より出やすい。
        ///       円錐の体積は同じ幅の球より小さい。注ぐ量が減って薄くならないよう密度を少し上げてある。
        FluidSource& source = addCone("Jet", { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.2f, 0.55f);
        source.density = 5.0f;
        source.temperature = 1.2f;
        source.velocity = { 0.0f, 2.0f, 0.0f };
        source.noise = 0.7f;
        /// @note 柱 1 本だと画面の真ん中に細い線が立つだけ。斜めの口を左右に足して裾を広げ、
        ///       高い所へ «雲の頭» を別に湧かせて repulse で潰す (上るほど太る蒸気の形)。
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& vent = addCone(side == 0 ? "Vent L" : "Vent R", { sign * 0.3f, -0.95f, 0.0f },
                                        { sign * 0.3f, 1.0f, 0.0f }, 0.13f, 0.38f);
            vent.density = 3.0f;
            vent.temperature = 0.8f;
            vent.velocity = { sign * 0.55f, 1.7f, 0.0f };
            vent.noise = 0.8f;
        }
        FluidSource& cap = addSource("Cap", { 0.0f, 0.25f, 0.0f }, 0.24f);
        cap.density = 1.6f;
        cap.temperature = 0.25f;
        cap.noise = 0.9f;
        FluidForce& billow = addForce("Billow", FluidForceType::Repulse, 1.1f);
        billow.center = { 0.0f, 0.2f, 0.0f };
        billow.radius = 0.8f;
        billow.falloffPower = 1.4f;
        FluidForce& wisps = addForce("Wisps", FluidForceType::Noise, 1.2f);
        wisps.noiseFrequency = 3.0f;
        gas.buoyancy = 1.1f;
        gas.vorticity = 0.4f;
        gas.turbulence = 0.45f;
        gas.densityDissipation = 0.6f;
        gas.temperatureDissipation = 0.8f;
        look.shading = FluidShading::Smoke;
        look.smokeColor = { 0.93f, 0.94f, 0.96f, 1.0f };
        look.shadowColor = { 0.55f, 0.58f, 0.63f, 1.0f };
        look.opacity = 2.8f;
        look.selfShadow = 1.2f;
        output.duration = 2.0f;
        output.warmup = 1.5f;
        output.substeps = 3;
        output.loop = true;
        break;
    }
    case FluidPreset::DustBurst: {
        /// @note 床に張り付いた爆発 (着地・衝撃の土煙)。横へ這わせるだけだと床に薄い帯が残って終わるので、
        ///       端をめくり上げてから浮かせ、立ち上がる壁にする。
        FluidSource& source = addSource("Impact", { 0.0f, -0.88f, 0.0f }, 0.2f);
        source.density = 2.5f;
        source.fuel = 7.0f;
        source.temperature = 0.7f;
        source.duration = 0.12f;
        source.noise = 0.8f;
        /// @note 床を «掃く» 円錐を左右へ寝かせ、逆回りの渦で外側の端をめくり上げる。押し出す力だけでは
        ///       薄い帯が床を這うだけで終わり、縦に何も無い絵になる。
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& sweep = addCone(side == 0 ? "Sweep L" : "Sweep R", { sign * 0.1f, -0.9f, 0.0f },
                                         { sign * 1.0f, 0.18f, 0.0f }, 0.16f, 0.7f);
            sweep.density = 4.6f;
            sweep.temperature = 0.25f;
            sweep.velocity = { sign * 2.2f, 0.4f, 0.0f };
            sweep.duration = 0.35f;
            sweep.noise = 0.85f;
            sweep.colorKey = 1.0f;
        }
        FluidSource& curlRing = addRing("Curl Ring", { 0.0f, -0.8f, 0.0f }, 0.58f, 0.12f);
        curlRing.density = 3.0f;
        curlRing.temperature = 0.2f;
        curlRing.startTime = 0.15f;
        curlRing.duration = 0.3f;
        curlRing.noise = 0.9f;
        curlRing.colorKey = 0.6f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& billow =
                addSource(side == 0 ? "Billow L" : "Billow R", { sign * 0.5f, -0.6f, 0.0f }, 0.18f);
            billow.density = 3.2f;
            billow.temperature = 0.5f;
            billow.velocity = { sign * 0.5f, 0.6f, 0.0f };
            billow.startTime = 0.2f;
            billow.duration = 0.35f;
            billow.noise = 0.9f;
            billow.colorKey = 0.35f;
        }
        FluidForce& shove = addForce("Shove", FluidForceType::Repulse, 3.5f);
        shove.center = { 0.0f, -0.9f, 0.0f };
        shove.radius = 1.1f;
        shove.falloffPower = 1.0f;
        shove.duration = 0.3f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidForce& roll = addForce(side == 0 ? "Roll L" : "Roll R", FluidForceType::Vortex, sign * 3.0f);
            roll.center = { sign * 0.6f, -0.75f, 0.0f };
            roll.direction = { 0.0f, 0.0f, 1.0f };
            roll.radius = 0.8f;
        }
        FluidForce& grit = addForce("Grit", FluidForceType::Noise, 1.2f);
        grit.noiseFrequency = 3.0f;
        grit.noiseSpeed = 0.7f;
        gas.floor = true;
        gas.buoyancy = 1.35f;
        gas.weight = 0.06f;
        gas.vorticity = 0.5f;
        gas.turbulence = 0.3f;
        gas.densityDissipation = 0.06f;
        gas.temperatureDissipation = 1.2f;
        gas.ignitionTemperature = 0.1f;
        gas.burnRate = 10.0f;
        gas.burnHeat = 0.3f;
        gas.burnSmoke = 2.4f;
        gas.burnExpansion = 4.0f;
        look.shading = FluidShading::Smoke;
        look.smokeColor = { 0.66f, 0.56f, 0.43f, 1.0f };
        look.shadowColor = { 0.22f, 0.17f, 0.12f, 1.0f };
        look.opacity = 3.5f;
        look.selfShadow = 2.0f;
        look.useAlbedoRamp = true;
        /// @note 鍵 0 = 焦げた芯、鍵 1 = 掃き出された乾いた土。
        look.albedoRamp = { { FluidColorStop{ { 0.070f, 0.055f, 0.040f }, 0.0f },
                              FluidColorStop{ { 0.180f, 0.140f, 0.095f }, 0.33f },
                              FluidColorStop{ { 0.320f, 0.245f, 0.160f }, 0.66f },
                              FluidColorStop{ { 0.480f, 0.370f, 0.240f }, 1.0f } } };
        output.duration = 2.0f;
        output.substeps = 3;
        break;
    }
    case FluidPreset::Ink: {
        /// @note 浮力なし・高い渦度。噴き出した勢いだけで巻き込む (水中のインク・霊気の渦)。
        ///       噴き出し口を横へ振る。動く速さが流れに乗り (inherit_velocity)、筋が片側へ巻き込む。
        ///       上下から逆向きに撃ち合わせるのは «ぶつかった所で巻く» ため。1 本だけだと筋が真上へ伸びて
        ///       途中で力尽き、タイルの半分が空のまま終わる。
        FluidSource& source = addSource("Nozzle", { 0.0f, -0.7f, 0.0f }, 0.1f);
        source.density = 8.0f;
        source.temperature = 0.0f;
        source.velocity = { 0.0f, 2.4f, 0.0f };
        source.duration = 0.8f;
        source.noise = 0.4f;
        source.motion.inheritVelocity = true;
        source.motion.keys = { FluidMotionKey{ 0.0f, { -0.25f, 0.0f, 0.0f } },
                               FluidMotionKey{ 0.8f, { 0.28f, 0.0f, 0.0f } } };
        FluidSource& counter = addSource("Counter Nozzle", { 0.0f, 0.7f, 0.0f }, 0.1f);
        counter.density = 6.0f;
        counter.temperature = 0.0f;
        counter.velocity = { 0.0f, -2.0f, 0.0f };
        counter.duration = 0.8f;
        counter.noise = 0.4f;
        counter.colorKey = 1.0f;
        counter.motion.inheritVelocity = true;
        counter.motion.keys = { FluidMotionKey{ 0.0f, { 0.25f, 0.0f, 0.0f } },
                                FluidMotionKey{ 0.8f, { -0.28f, 0.0f, 0.0f } } };
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& bloom = addSource(side == 0 ? "Bloom L" : "Bloom R", { sign * 0.55f, 0.0f, 0.0f }, 0.12f);
            bloom.density = 4.0f;
            bloom.temperature = 0.0f;
            bloom.velocity = { sign * 1.1f, sign * -0.3f, 0.0f };
            bloom.startTime = 0.25f;
            bloom.duration = 0.4f;
            bloom.noise = 0.6f;
            bloom.colorKey = 0.5f;
        }
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidForce& curl = addForce(side == 0 ? "Curl L" : "Curl R", FluidForceType::Vortex, sign * -2.2f);
            curl.center = { sign * 0.35f, 0.0f, 0.0f };
            curl.direction = { 0.0f, 0.0f, 1.0f };
            curl.radius = 0.8f;
            curl.falloffPower = 1.5f;
        }
        FluidForce& feather = addForce("Feather", FluidForceType::Noise, 1.0f);
        feather.noiseFrequency = 2.5f;
        feather.noiseSpeed = 0.5f;
        gas.buoyancy = 0.0f;
        gas.weight = 0.0f;
        gas.vorticity = 1.0f;
        gas.turbulence = 0.2f;
        gas.densityDissipation = 0.03f;
        gas.temperatureDissipation = 0.0f;
        gas.velocityDamping = 0.35f;
        look.shading = FluidShading::Smoke;
        look.smokeColor = { 0.10f, 0.12f, 0.30f, 1.0f };
        look.shadowColor = { 0.02f, 0.02f, 0.06f, 1.0f };
        look.opacity = 6.0f;
        look.selfShadow = 0.6f;
        look.detailStrength = 0.2f;
        look.useAlbedoRamp = true;
        /// @note 鍵 0 = 下から撃つ藍、鍵 1 = 上から落ちる紫。ぶつかった所で 2 色が混ざる。
        look.albedoRamp = { { FluidColorStop{ { 0.030f, 0.045f, 0.140f }, 0.0f },
                              FluidColorStop{ { 0.070f, 0.060f, 0.180f }, 0.33f },
                              FluidColorStop{ { 0.130f, 0.055f, 0.200f }, 0.66f },
                              FluidColorStop{ { 0.210f, 0.060f, 0.190f }, 1.0f } } };
        output.duration = 3.0f;
        output.substeps = 3;
        break;
    }
    case FluidPreset::MagicWisp: {
        /// @note 柱 + 輪 + 周回する 2 つの玉。玉は motion のキーで «湧き口ごと» 回るので、
        ///       力で回すのと違って輪郭が保たれたまま動く。
        FluidSource& source = addSource("Wisp", { 0.0f, -0.6f, 0.0f }, 0.16f);
        source.density = 3.5f;
        source.temperature = 1.4f;
        source.velocity = { 0.0f, 0.9f, 0.0f };
        source.noise = 0.9f;
        FluidSource& halo = addRing("Halo", { 0.0f, -0.1f, 0.0f }, 0.55f, 0.1f);
        halo.density = 2.0f;
        halo.temperature = 1.0f;
        halo.noise = 1.0f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& mote = addSource(side == 0 ? "Mote L" : "Mote R", { sign * 0.5f, -0.35f, 0.0f }, 0.1f);
            mote.density = 1.8f;
            mote.temperature = 0.8f;
            mote.noise = 1.0f;
            mote.motion.inheritVelocity = true;
            /// @note 端と端が同じ位置なので、ループの継ぎ目で玉が跳ばない。
            mote.motion.keys = { FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                                 FluidMotionKey{ 1.75f, { sign * -0.25f, 0.75f, 0.0f } },
                                 FluidMotionKey{ 3.5f, { 0.0f, 0.0f, 0.0f } } };
        }
        /// @note 立ち上る柱をゆっくりねじる渦。半径の外では効かないので形は崩さない。
        FluidForce& swirl = addForce("Swirl", FluidForceType::Vortex, 1.6f);
        swirl.center = { 0.0f, -0.1f, 0.0f };
        swirl.direction = { 0.0f, 1.0f, 0.0f };
        swirl.radius = 0.9f;
        swirl.falloffPower = 2.0f;
        FluidForce& bloom = addForce("Bloom", FluidForceType::Repulse, 0.9f);
        bloom.center = { 0.0f, -0.1f, 0.0f };
        bloom.radius = 0.9f;
        bloom.falloffPower = 1.6f;
        FluidForce& shimmer = addForce("Shimmer", FluidForceType::Noise, 1.8f);
        shimmer.noiseFrequency = 4.5f;
        shimmer.noiseSpeed = 1.2f;
        gas.buoyancy = 0.9f;
        gas.vorticity = 1.0f;
        gas.turbulence = 0.7f;
        gas.turbulenceScale = 5.0f;
        gas.densityDissipation = 0.45f;
        gas.temperatureDissipation = 0.8f;
        look.shading = FluidShading::Glow;
        look.smokeColor = { 0.35f, 0.7f, 1.0f, 1.0f };
        look.opacity = 3.2f;
        look.useEmissionRamp = true;
        /// @note 冷えた縁の藍から芯の白へ。HDR (1 を超える) のままにして Bloom を掛ける。
        look.emissionRamp = { { FluidColorStop{ { 0.02f, 0.05f, 0.18f }, 0.0f },
                                FluidColorStop{ { 0.10f, 0.55f, 1.60f }, 0.35f },
                                FluidColorStop{ { 0.45f, 1.60f, 2.60f }, 0.7f },
                                FluidColorStop{ { 2.40f, 3.40f, 4.20f }, 1.0f } } };
        output.duration = 2.0f;
        output.warmup = 1.5f;
        output.substeps = 3;
        output.loop = true;
        break;
    }
    case FluidPreset::HeatHaze: {
        /// @note 見えない熱気の流れを歪みマップへ。密度は «どこが揺らぐか» のマスクとしてだけ使う。
        ///       歪みは色を持たないので、揺れる «面積» がそのまま効き目になる。細い柱ではなく
        ///       床いっぱいの熱の床から立ち上げる。
        FluidSource& source = addSource("Heat Bed", { 0.0f, -0.85f, 0.0f }, 0.7f);
        source.shape = FluidSourceShape::Box;
        source.size = { 0.7f, 0.12f, 0.3f };
        source.density = 2.6f;
        source.temperature = 1.2f;
        source.velocity = { 0.0f, 0.8f, 0.0f };
        source.noise = 0.7f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& column = addCone(side == 0 ? "Column L" : "Column R", { sign * 0.5f, -0.9f, 0.0f },
                                          { sign * 0.2f, 1.0f, 0.0f }, 0.18f, 0.6f);
            column.density = 2.0f;
            column.temperature = 1.0f;
            column.velocity = { sign * 0.3f, 1.1f, 0.0f };
            column.noise = 0.9f;
        }
        FluidForce& shimmer = addForce("Shimmer", FluidForceType::Noise, 2.2f);
        shimmer.noiseFrequency = 5.0f;
        shimmer.noiseSpeed = 1.4f;
        FluidForce& drift = addForce("Drift", FluidForceType::Wind, 0.5f);
        drift.direction = { 1.0f, 0.15f, 0.0f };
        FluidForce& spread = addForce("Spread", FluidForceType::Repulse, 0.8f);
        spread.center = { 0.0f, -0.1f, 0.0f };
        spread.radius = 1.0f;
        spread.falloffPower = 1.4f;
        gas.buoyancy = 1.6f;
        gas.vorticity = 0.5f;
        gas.turbulence = 0.6f;
        gas.turbulenceScale = 4.0f;
        gas.densityDissipation = 0.35f;
        look.shading = FluidShading::Distortion;
        look.opacity = 2.5f;
        output.duration = 2.0f;
        output.warmup = 1.5f;
        output.substeps = 3;
        output.loop = true;
        output.motionVectors = false;
        break;
    }
    case FluidPreset::WaterSplash: {
        /// @note 芯の柱 + 左右の王冠 + 上から降る霧。spread (ばらつき) は «撃ち出す速さ × spread» の
        ///       横向きの当たりくじで、1 秒あれば当たった粒はタイルの外へ出るため小さく保ち、横幅は
        ///       別の湧き口で作る。寿命を付けるのは、床に落ちた粒の溜まりが «粒子の数 ÷ 高さ» の幅まで
        ///       必ず広がるため。飛沫が消えていけば、溜まりが枠を越える前に絵が終わる。
        FluidSource& source = addLiquidSource("Splash", { 0.0f, -0.85f, 0.0f }, 0.15f);
        source.velocity = { 0.0f, 3.6f, 0.0f };
        source.spread = 0.16f;
        source.count = 560;
        source.duration = 0.16f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& crown =
                addLiquidSource(side == 0 ? "Crown L" : "Crown R", { sign * 0.42f, -0.9f, 0.0f }, 0.14f);
            crown.velocity = { 0.0f, 2.4f, 0.0f };
            crown.spread = 0.15f;
            crown.count = 90;
            crown.startTime = 0.1f;
            crown.duration = 0.35f;
        }
        FluidSource& mist = addLiquidSource("Mist", { 0.0f, 0.45f, 0.0f }, 0.55f);
        mist.velocity = { 0.0f, -0.2f, 0.0f };
        mist.spread = 0.3f;
        mist.count = 110;
        mist.startTime = 0.15f;
        mist.duration = 0.5f;
        liquid.particleRadius = 0.011f;
        liquid.gravity = 7.0f;
        liquid.viscosity = 0.02f;
        liquid.cohesion = 0.25f;
        liquid.solverIterations = 6;
        liquid.floorHeight = -0.95f;
        liquid.floorFriction = 0.95f;
        liquid.particleLifetime = 0.85f;
        look.liquidColor = { 0.40f, 0.65f, 0.95f, 0.8f };
        look.specular = 0.9f;
        output.duration = 1.1f;
        break;
    }
    case FluidPreset::WaterJet: {
        FluidSource& source = addLiquidSource("Jet", { -0.85f, -0.3f, 0.0f }, 0.04f);
        source.velocity = { 2.4f, 1.4f, 0.0f };
        source.spread = 0.08f;
        source.count = 1600;
        source.duration = 1.6f;
        /// @note 芯の弧に «ばらけた霧» を重ねて 1 本の線に見えないようにする。ばらつきは小さく保つ
        ///       (大きくすると後ろ向きに飛ぶ粒が出て、口の左側がタイルからはみ出す)。
        FluidSource& spray = addLiquidSource("Spray", { -0.82f, -0.28f, 0.0f }, 0.07f);
        spray.velocity = { 2.1f, 1.5f, 0.0f };
        spray.spread = 0.22f;
        spray.count = 600;
        spray.duration = 1.6f;
        /// @note 岩の天面から跳ね返る水。ほぼ真上に撃つ (横へ撃つと右端から出ていく)。
        FluidSource& rebound = addLiquidSource("Rebound", { 0.62f, -0.62f, 0.0f }, 0.12f);
        rebound.velocity = { 0.0f, 2.2f, 0.0f };
        rebound.spread = 0.4f;
        rebound.count = 700;
        rebound.startTime = 0.25f;
        rebound.duration = 1.35f;
        /// @note 弧が床の少し上を横切る所 (x ≈ 0.7) に岩を置く。噴流が天面で砕けて両側へこぼれる。
        ///       床 (floorHeight = -0.95) に接地させ、下をくぐる隙間を作らない。
        FluidCollider rock;
        rock.name = "Rock";
        rock.shape = FluidColliderShape::Box;
        rock.center = { 0.7f, -0.8f, 0.0f };
        rock.size = { 0.15f, 0.15f, 0.3f };
        recipe.colliders.push_back(std::move(rock));
        liquid.gravity = 6.0f;
        liquid.viscosity = 0.03f;
        liquid.cohesion = 0.35f;
        liquid.floorHeight = -0.95f;
        liquid.floorFriction = 0.8f;
        look.liquidColor = { 0.40f, 0.65f, 0.95f, 0.8f };
        look.specular = 0.9f;
        output.duration = 0.8f;
        output.warmup = 0.8f;
        output.loop = true;
        break;
    }
    case FluidPreset::BloodBurst: {
        /// @note 全方位へ飛ぶ粘い雫。床は置かず、寿命で細りながら消す (被弾の飛沫)。床が無いので、
        ///       撃ち出す速さは遅く保つ (速い粒はそのままタイルの外へ出て二度と戻らない)。«大きさ» は
        ///       速度ではなく «湧き口の広さ» と «数» で作る (粒子どうしが押し合って広がる)。
        FluidSource& source = addLiquidSource("Burst", { 0.0f, 0.2f, 0.0f }, 0.26f);
        source.velocity = { 0.0f, 0.3f, 0.0f };
        source.spread = 0.5f;
        source.count = 400;
        source.duration = 0.1f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& spray =
                addLiquidSource(side == 0 ? "Spray L" : "Spray R", { sign * 0.42f, 0.1f, 0.0f }, 0.14f);
            spray.velocity = { sign * 0.45f, 0.25f, 0.0f };
            spray.spread = 0.25f;
            spray.count = 160;
            spray.duration = 0.14f;
        }
        FluidSource& gout = addLiquidSource("Gout", { 0.0f, -0.05f, 0.0f }, 0.18f);
        gout.velocity = { 0.0f, -0.2f, 0.0f };
        gout.spread = 0.25f;
        gout.count = 160;
        gout.startTime = 0.08f;
        gout.duration = 0.3f;
        liquid.gravity = 0.8f;
        liquid.viscosity = 0.2f;
        liquid.cohesion = 0.6f;
        liquid.floor = false;
        liquid.particleLifetime = 1.1f;
        look.liquidColor = { 0.42f, 0.02f, 0.03f, 0.95f };
        look.specular = 0.6f;
        /// @note 3D の液面: 血は濃く (透けない)、艶は水より鈍い。
        look.liquidExtinction = 120.0f;
        look.liquidGloss = 64.0f;
        output.duration = 0.9f;
        break;
    }
    case FluidPreset::LavaBlob: {
        /// @note 噴き上げた塊が落ちて、床いっぱいに広がる «溜まり» になる。粒子 1 つは «その面積» を
        ///       要求し、床に落ちた溜まりの広さはほぼ «数 × 粒子の断面» で決まるので、粒子を増やすと
        ///       タイルの左右から溢れる (高さは増えない)。
        FluidSource& source = addLiquidSource("Blob", { 0.0f, -0.8f, 0.0f }, 0.12f);
        source.velocity = { 0.0f, 2.6f, 0.0f };
        source.spread = 0.3f;
        source.count = 190;
        source.duration = 0.15f;
        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            FluidSource& lob = addLiquidSource(side == 0 ? "Lob L" : "Lob R", { sign * 0.28f, -0.85f, 0.0f }, 0.1f);
            lob.velocity = { sign * 0.25f, 2.5f, 0.0f };
            lob.spread = 0.25f;
            lob.count = 95;
            lob.startTime = 0.05f;
            lob.duration = 0.18f;
        }
        FluidSource& spatter = addLiquidSource("Spatter", { 0.0f, -0.7f, 0.0f }, 0.16f);
        spatter.velocity = { 0.0f, 1.8f, 0.0f };
        spatter.spread = 0.5f;
        spatter.count = 70;
        spatter.startTime = 0.12f;
        spatter.duration = 0.3f;
        liquid.particleRadius = 0.014f;
        liquid.gravity = 5.0f;
        liquid.viscosity = 0.8f;
        liquid.cohesion = 0.9f;
        liquid.floorHeight = -0.95f;
        liquid.floorFriction = 0.95f;
        look.liquidColor = { 1.0f, 0.38f, 0.06f, 1.0f };
        look.specular = 0.5f;
        /// @note 3D の液面: 溶岩は不透明で縁が柔らかい。
        look.liquidExtinction = 160.0f;
        look.liquidSoftness = 0.12f;
        look.liquidGloss = 32.0f;
        output.duration = 1.4f;
        break;
    }
    case FluidPreset::PlasmaBurst: {
        /// @note 放電が弾けた «瞬間» の空気。稲妻の線そのものは VFX Line の担当で、ここは線の周りだけ。
        ///       芯 → 輪 → 上下へ抜ける枝の 3 段で、0.8 秒のうちにタイルの端まで届いて消える。
        FluidSource& source = addSource("Core", { 0.0f, 0.0f, 0.0f }, 0.14f);
        source.density = 7.0f;
        source.temperature = 4.0f;
        source.duration = 0.08f;
        source.noise = 0.4f;
        /// @note 輪の法線を奥行き軸にして «こちらを向いた輪» にする (上向きだと真横から見た線になる)。
        FluidSource& shell = addRing("Shell", { 0.0f, 0.0f, 0.0f }, 0.4f, 0.12f);
        shell.direction = { 0.0f, 0.0f, 1.0f };
        shell.density = 4.0f;
        shell.temperature = 2.5f;
        shell.startTime = 0.04f;
        shell.duration = 0.14f;
        shell.noise = 0.9f;
        struct Branch {
            const char* name;
            math::Vector3 direction;
            math::Vector3 velocity;
        };
        /// @note 上下で傾きを揃えない。左右対称だと «X 字» に見えて、枝分かれではなく図形になる。
        const Branch kBranches[] = { { "Branch Up", { 0.25f, 1.0f, 0.0f }, { 0.5f, 2.2f, 0.0f } },
                                     { "Branch Down", { -0.3f, -1.0f, 0.0f }, { -0.6f, -2.2f, 0.0f } } };
        for (const Branch& branch : kBranches) {
            FluidSource& arm = addCone(branch.name, { 0.0f, 0.0f, 0.0f }, branch.direction, 0.16f, 0.8f);
            arm.density = 3.0f;
            arm.temperature = 2.0f;
            arm.velocity = branch.velocity;
            arm.duration = 0.12f;
            arm.noise = 1.0f;
        }
        FluidForce& discharge = addForce("Discharge", FluidForceType::Repulse, 7.0f);
        discharge.radius = 1.2f;
        discharge.falloffPower = 0.9f;
        discharge.duration = 0.22f;
        FluidForce& fork = addForce("Fork", FluidForceType::Noise, 3.0f);
        fork.noiseFrequency = 6.0f;
        fork.noiseSpeed = 2.5f;
        /// @note 押し切ったあとに止める。減速が無いと «弾けた» ではなく «流れ去った» に見える。
        FluidForce& settle = addForce("Settle", FluidForceType::Drag, 4.0f);
        settle.startTime = 0.3f;
        gas.buoyancy = 0.25f;
        gas.weight = 0.0f;
        gas.vorticity = 0.8f;
        gas.turbulence = 0.5f;
        gas.turbulenceScale = 6.0f;
        gas.densityDissipation = 1.2f;
        gas.temperatureDissipation = 2.6f;
        gas.velocityDamping = 0.6f;
        look.shading = FluidShading::Glow;
        look.smokeColor = { 0.55f, 0.75f, 1.0f, 1.0f };
        look.opacity = 5.0f;
        look.useEmissionRamp = true;
        /// @note 青 → 白。白の側は 1 を大きく超える HDR にして、Bloom で «放電» の明るさを出す。
        look.emissionRamp = { { FluidColorStop{ { 0.05f, 0.12f, 0.45f }, 0.0f },
                                FluidColorStop{ { 0.35f, 0.95f, 2.60f }, 0.4f },
                                FluidColorStop{ { 1.80f, 3.20f, 5.50f }, 0.75f },
                                FluidColorStop{ { 5.50f, 6.20f, 7.00f }, 1.0f } } };
        output.duration = 0.8f;
        output.substeps = 4;
        output.loop = false;
        break;
    }
    case FluidPreset::ArcHaze: {
        /// @note 放電の残り香。細く長く漂う靄。形を崩すのは noise の力で、位置を運ぶのは玉の motion。
        FluidSource& source = addCone("Trail", { 0.0f, -0.95f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.16f, 0.7f);
        source.density = 2.4f;
        source.temperature = 1.2f;
        source.velocity = { 0.0f, 1.1f, 0.0f };
        source.noise = 1.0f;
        FluidSource& veil = addSource("Veil", { 0.0f, -0.4f, 0.0f }, 0.55f);
        veil.shape = FluidSourceShape::Box;
        veil.size = { 0.55f, 0.09f, 0.3f };
        veil.density = 1.2f;
        veil.temperature = 0.8f;
        veil.noise = 1.0f;
        struct Ember {
            const char* name;
            float x;
            math::Vector3 rest;
            math::Vector3 swing;
        };
        /// @note 端 (0 と 3.5) が同じ位置なので、ループの継ぎ目で玉が跳ばない。左右は位相を逆に取る。
        const Ember kEmbers[] = { { "Ember L", -0.55f, { 0.0f, -0.15f, 0.0f }, { 0.3f, 0.35f, 0.0f } },
                                  { "Ember R", 0.55f, { 0.0f, 0.35f, 0.0f }, { -0.3f, -0.15f, 0.0f } } };
        for (const Ember& ember : kEmbers) {
            FluidSource& mote = addSource(ember.name, { ember.x, 0.05f, 0.0f }, 0.12f);
            mote.density = 2.0f;
            mote.temperature = 1.0f;
            mote.noise = 1.0f;
            mote.motion.inheritVelocity = true;
            mote.motion.keys = { FluidMotionKey{ 0.0f, ember.rest },
                                 FluidMotionKey{ 1.75f, ember.swing },
                                 FluidMotionKey{ 3.5f, ember.rest } };
        }
        FluidForce& tear = addForce("Tear", FluidForceType::Noise, 2.0f);
        tear.noiseFrequency = 5.5f;
        tear.noiseSpeed = 1.6f;
        FluidForce& spread = addForce("Spread", FluidForceType::Repulse, 0.5f);
        spread.center = { 0.0f, -0.15f, 0.0f };
        spread.radius = 1.1f;
        spread.falloffPower = 1.2f;
        FluidForce& creep = addForce("Creep", FluidForceType::Vortex, 0.9f);
        creep.center = { 0.0f, 0.1f, 0.0f };
        creep.direction = { 0.0f, 0.0f, 1.0f };
        creep.radius = 1.0f;
        creep.falloffPower = 1.5f;
        gas.buoyancy = 0.55f;
        gas.weight = 0.0f;
        gas.vorticity = 1.2f;
        gas.turbulence = 0.8f;
        gas.turbulenceScale = 6.0f;
        gas.densityDissipation = 0.85f;
        gas.temperatureDissipation = 0.7f;
        gas.velocityDamping = 0.25f;
        look.shading = FluidShading::Glow;
        look.smokeColor = { 0.42f, 0.68f, 1.0f, 1.0f };
        look.opacity = 4.5f;
        look.useEmissionRamp = true;
        look.emissionRamp = { { FluidColorStop{ { 0.02f, 0.06f, 0.20f }, 0.0f },
                                FluidColorStop{ { 0.08f, 0.35f, 1.00f }, 0.4f },
                                FluidColorStop{ { 0.35f, 1.10f, 2.20f }, 0.75f },
                                FluidColorStop{ { 1.60f, 2.40f, 3.60f }, 1.0f } } };
        output.duration = 2.4f;
        output.warmup = 1.5f;
        output.substeps = 3;
        output.loop = true;
        break;
    }
    case FluidPreset::GroundRing: {
        FluidSource& ring = addRing("Ground Front", { 0.0f, -0.35f, 0.0f }, 0.28f, 0.07f);
        ring.direction = { 0.0f, 0.0f, 1.0f };
        ring.density = 12.0f;
        ring.temperature = 0.2f;
        ring.duration = 0.1f;
        FluidForce& spread = addForce("Impact Expansion", FluidForceType::Repulse, 6.0f);
        spread.center = ring.center;
        spread.radius = 1.5f;
        spread.duration = 0.25f;
        addForce("Settle", FluidForceType::Drag, 2.0f).startTime = 0.3f;
        gas.buoyancy = 0.0f;
        gas.weight = 0.1f;
        gas.densityDissipation = 1.8f;
        look.smokeColor = { 0.42f, 0.32f, 0.22f, 1.0f };
        look.opacity = 4.0f;
        output.duration = 1.1f;
        break;
    }
    case FluidPreset::ColdMist: {
        FluidSource& mist = addSource("Cold Veil", { 0.0f, -0.55f, 0.0f }, 0.4f);
        mist.shape = FluidSourceShape::Box;
        mist.size = { 0.5f, 0.08f, 0.25f };
        mist.density = 5.0f;
        mist.temperature = 0.0f;
        mist.duration = 0.45f;
        mist.velocity = { 0.0f, 0.25f, 0.0f };
        addForce("Crawl", FluidForceType::Noise, 0.8f).noiseFrequency = 4.0f;
        FluidForce& spread = addForce("Spread", FluidForceType::Repulse, 1.0f);
        spread.center = mist.center;
        spread.radius = 1.2f;
        gas.buoyancy = 0.0f;
        gas.weight = 0.1f;
        gas.floor = true;
        gas.densityDissipation = 1.2f;
        look.smokeColor = { 0.65f, 0.85f, 1.0f, 1.0f };
        look.opacity = 2.5f;
        output.duration = 2.0f;
        break;
    }
    case FluidPreset::ChargeVortex: {
        FluidSource& ring = addRing("Gathering Ring", {}, 0.65f, 0.09f);
        ring.direction = { 0.0f, 0.0f, 1.0f };
        ring.density = 6.0f;
        ring.temperature = 1.0f;
        ring.duration = 0.7f;
        ring.amount.keys = { { 0.0f, 0.1f }, { 0.3f, 1.0f }, { 0.7f, 0.0f } };
        FluidForce& pull = addForce("Pull Into Core", FluidForceType::Attract, 4.0f);
        pull.radius = 1.4f;
        FluidForce& spin = addForce("Spiral", FluidForceType::Vortex, 2.5f);
        spin.direction = { 0.0f, 0.0f, 1.0f };
        spin.radius = 1.2f;
        gas.buoyancy = 0.0f;
        gas.weight = 0.0f;
        gas.densityDissipation = 1.4f;
        look.shading = FluidShading::Glow;
        look.smokeColor = { 0.25f, 0.55f, 1.0f, 1.0f };
        look.opacity = 5.0f;
        output.duration = 1.0f;
        break;
    }
    case FluidPreset::EmberBurst: {
        for (int i = 0; i < 5; ++i) {
            const float x = static_cast<float>(i - 2) * 0.16f;
            const std::string name = "Ember " + std::to_string(i);
            FluidSource& ember = addSource(name.c_str(), { x, -0.4f, 0.0f }, 0.06f);
            ember.density = 8.0f;
            ember.temperature = 3.0f;
            ember.duration = 0.12f;
            ember.velocity = { x * 3.0f, 1.2f + 0.2f * static_cast<float>(i % 2), 0.0f };
        }
        addForce("Break Up", FluidForceType::Noise, 1.0f).noiseFrequency = 7.0f;
        gas.buoyancy = 0.2f;
        gas.weight = 0.0f;
        gas.densityDissipation = 2.2f;
        look.shading = FluidShading::Glow;
        look.smokeColor = { 1.0f, 0.28f, 0.03f, 1.0f };
        look.opacity = 6.0f;
        output.duration = 1.2f;
        break;
    }
    case FluidPreset::SigilFlare: {
        FluidSource& outer = addRing("Outer Seal", {}, 0.55f, 0.045f);
        outer.direction = { 0.0f, 0.0f, 1.0f };
        outer.density = 12.0f;
        outer.temperature = 2.0f;
        outer.duration = 0.22f;
        FluidSource& inner = addRing("Inner Seal", {}, 0.3f, 0.04f);
        inner.direction = { 0.0f, 0.0f, 1.0f };
        inner.density = 10.0f;
        inner.temperature = 2.0f;
        inner.startTime = 0.08f;
        inner.duration = 0.2f;
        addForce("Release", FluidForceType::Repulse, 2.0f).startTime = 0.25f;
        addForce("Fray", FluidForceType::Noise, 0.6f).noiseFrequency = 6.0f;
        gas.buoyancy = 0.0f;
        gas.weight = 0.0f;
        gas.densityDissipation = 2.0f;
        look.shading = FluidShading::Glow;
        look.smokeColor = { 0.7f, 0.25f, 1.0f, 1.0f };
        look.opacity = 6.0f;
        output.duration = 1.4f;
        break;
    }
    case FluidPreset::Count:
    default:
        addSource("Source", { 0.0f, -0.6f, 0.0f }, 0.18f);
        break;
    }

    /// @note 焼き方の既定。すべて 3D で焼く (6 方向ライトマップで光が回り込み、焼いた後で光の向きを変えられる)。
    ///       以前ループものと陽炎を 2D に残していたのは、3D の焼きが末尾→先頭のクロスフェードと歪みマップを
    ///       持たなかったため。どちらも 3D で焼けるようになり、2D に残す理由が無くなった。
    FluidBakeSettings& bake = recipe.bake;
    bake.mode = FluidBakeMode::Volume3D;
    if (recipe.kind == FluidKind::Gas) {
        /// @note 煙の縁の瞬きは超解像でしか消えない。2D も 3D も [output] の同じ値を使う。
        output.supersampling = 2;
        bake.volumeResolution = 128;
        /// @note 発光だけの Glow は光を受けず、歪みマップは色を焼かない。どちらも 6 方向の陰影を焼いても使われない。
        bake.sixWayLightmaps = look.shading == FluidShading::Smoke || look.shading == FluidShading::Fire;
    } else {
        /// @note CPU の粒子ソルバーの上限は 96³。粒子の太さに対して 64 で液面が足りる。
        bake.volumeResolution = 64;
        /// @note GPU の粒子ソルバーは入ったばかりで、CPU と同じ絵になるかをまだ見比べていない。
        ///       出発点になるプリセットは確かめた CPU に置き、gpu はユーザーが選んで試す。
        bake.solver = FluidBakeSolver::Cpu;
    }
    return recipe;
}

bool LoadFluidRecipe(const std::string& absPath, FluidRecipe& outRecipe, std::string* outError)
{
    std::ifstream stream(util::FileSystem::PathFromUtf8(absPath), std::ios::binary);
    if (!stream) {
        if (outError != nullptr) *outError = "開けません: " + absPath;
        return false;
    }
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();
    toml::parse_result parsed = toml::parse(text, absPath);
    if (!parsed) {
        if (outError != nullptr) *outError = std::string(parsed.error().description());
        return false;
    }
    const toml::table& root = parsed.table();
    const NodeView view{ &root };

    FluidRecipe recipe;
    const int64_t version = view["version"].value_or(static_cast<int64_t>(1));
    recipe.kind = KindFromName(view["kind"].value_or(std::string(KindName(recipe.kind))));
    recipe.seed = static_cast<uint32_t>(view["seed"].value_or(static_cast<int64_t>(recipe.seed)));
    ReadGas(view["gas"], recipe.gas);
    ReadLiquid(view["liquid"], recipe.liquid);
    ReadRender(view["render"], recipe.render);
    ReadOutput(view["output"], recipe.output);
    ReadBake(view["bake"], recipe.bake);
    const auto maxSources = static_cast<std::size_t>(kMaxFluidSources);
    if (version < 2) {
        /// @note version 1 は気体と液体で別のリストを持ち、焼くときは今の kind の側しか見ていなかった。
        ///       使われていなかった側まで移すと、kind を切り替えただけで知らない発生源が湧く。
        if (recipe.kind == FluidKind::Liquid)
            ReadTableArray(view["liquid_emitter"], maxSources, recipe.sources, ReadV1LiquidEmitter);
        else
            ReadTableArray(view["gas_source"], maxSources, recipe.sources, ReadV1GasSource);
    } else {
        ReadTableArray(view["source"], maxSources, recipe.sources, ReadSource);
        ReadTableArray(view["force"], static_cast<std::size_t>(kMaxFluidForces), recipe.forces, ReadForce);
        ReadTableArray(view["collider"], static_cast<std::size_t>(kMaxFluidColliders), recipe.colliders,
                       ReadCollider);
    }
    outRecipe = std::move(recipe);
    return true;
}

bool SaveFluidRecipe(const std::string& absPath, const FluidRecipe& recipe)
{
    toml::table root;
    root.insert("version", kFormatVersion);
    root.insert("kind", std::string(KindName(recipe.kind)));
    root.insert("seed", static_cast<int64_t>(recipe.seed));
    root.insert("gas", WriteGas(recipe.gas));
    root.insert("liquid", WriteLiquid(recipe.liquid));
    root.insert("render", WriteRender(recipe.render));
    root.insert("output", WriteOutput(recipe.output));
    root.insert("bake", WriteBake(recipe.bake));
    WriteTableArray(root, "source", recipe.sources, WriteSource);
    WriteTableArray(root, "force", recipe.forces, WriteForce);
    WriteTableArray(root, "collider", recipe.colliders, WriteCollider);

    std::ostringstream out;
    out << root << '\n';
    return util::FileSystem::WriteText(absPath, out.str());
}

void ReflectFluidRecipe(FluidRecipe& recipe, scene::IReflector& r)
{
    r.Group("Recipe");
    ReflectEnum(r, "kind", recipe.kind, kKindLabels);
    /// @note IReflector に符号なしの Field は無い。読むだけのリフレクタで値を壊さないよう、変わったときだけ書き戻す。
    int seed = static_cast<int>(recipe.seed);
    r.Field("seed", seed);
    if (seed != static_cast<int>(recipe.seed)) recipe.seed = static_cast<uint32_t>((std::max)(seed, 0));
    /// @note kind の読み替えで意味の変わる項目 (気体の注入量 / 液体の撃ち出し数) だけを出し分ける。
    const bool liquid = recipe.kind == FluidKind::Liquid;

    r.Group("Gas");
    r.BeginObject("gas");
    ReflectGas(recipe.gas, r);
    r.EndObject();

    r.Group("Liquid");
    r.BeginObject("liquid");
    ReflectLiquid(recipe.liquid, r);
    r.EndObject();

    r.Group("Sources");
    ReflectList(r, "source", recipe.sources, static_cast<std::size_t>(kMaxFluidSources),
                [&r, liquid](FluidSource& source) { ReflectSource(source, liquid, r); });

    r.Group("Forces");
    ReflectList(r, "force", recipe.forces, static_cast<std::size_t>(kMaxFluidForces),
                [&r](FluidForce& force) { ReflectForce(force, r); });

    r.Group("Colliders");
    ReflectList(r, "collider", recipe.colliders, static_cast<std::size_t>(kMaxFluidColliders),
                [&r, liquid](FluidCollider& collider) { ReflectCollider(collider, liquid, r); });

    r.Group("Render");
    r.BeginObject("render");
    ReflectRender(recipe.render, liquid, r);
    r.EndObject();

    r.Group("Output");
    r.BeginObject("output");
    ReflectOutput(recipe.output, r);
    r.EndObject();

    r.Group("Bake");
    r.BeginObject("bake");
    ReflectBake(recipe.bake, r);
    r.EndObject();
}

} /// @note namespace fbzz::asset
