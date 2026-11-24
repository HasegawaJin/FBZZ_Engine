/// @file    FluidRecipeReflection.cpp
/// @brief   流体レシピの Inspector と編集 API 向けの項目定義。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include <Engine/Asset/FluidRecipeCodec.hpp>

#include "FluidRecipeLabels.hpp"

#include <Engine/Scene/Reflection.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace fbzz::asset {
namespace {

using namespace fbzz::fluid;
namespace labels = fluid_recipe_labels;

/// @note 読むだけのリフレクタに通しても値を変えないよう、変わったときだけ書き戻す。
template <typename Enum, std::size_t LABEL_COUNT>
void ReflectEnum(scene::IReflector& reflector, const char* name, Enum& value, const char* const (&labels)[LABEL_COUNT])
{
    const int originalIndex = static_cast<int>(value);
    int index = originalIndex;
    reflector.Enum(name, index, std::span<const char* const>(labels));
    if (index != originalIndex) {
        value = static_cast<Enum>(std::clamp(index, 0, static_cast<int>(LABEL_COUNT) - 1));
    }
}

/// @note FieldIf の範囲つき版。FieldIf は Field へ流すので、JSON 側の範囲の丸めが効かなくなる。
void FloatRangeIf(scene::IReflector& reflector, const char* name, float& value, float minValue, float maxValue,
                  bool isVisible, const char* tooltip = nullptr)
{
    reflector.BeginField(name, name);
    reflector.SetFieldVisible(isVisible);
    reflector.FloatRange(name, value, minValue, maxValue);
    if (isVisible && tooltip != nullptr) reflector.Tooltip(tooltip);
    reflector.EndField();
}

void IntRangeIf(scene::IReflector& reflector, const char* name, int& value, int minValue, int maxValue, bool isVisible,
                const char* tooltip = nullptr)
{
    reflector.BeginField(name, name);
    reflector.SetFieldVisible(isVisible);
    reflector.IntRange(name, value, minValue, maxValue);
    if (isVisible && tooltip != nullptr) reflector.Tooltip(tooltip);
    reflector.EndField();
}

/// @brief 要素数は maxCount で切る (Inspector の Add・AI の長い配列のどちらも)。
/// @note 部品の数は GPU の定数バッファに載る数で決まっており、CPU だけ多く置けると焼き分けで絵が変わる。
template <typename T, typename ReflectElement>
void ReflectList(scene::IReflector& reflector, const char* name, std::vector<T>& items, std::size_t maxCount,
                 ReflectElement reflectElement)
{
    const std::size_t count = (std::min)(reflector.BeginObjectList(name, items.size()), maxCount);
    items.resize(count);
    for (std::size_t index = 0; index < items.size(); ++index) {
        reflector.BeginObjectElement(index);
        reflectElement(items[index]);
        reflector.EndObjectElement();
    }
    const std::size_t removeIndex = reflector.EndObjectList();
    if (removeIndex < items.size()) {
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        return;
    }
    std::size_t fromIndex = 0;
    std::size_t toIndex = 0;
    if (reflector.ObjectListMove(fromIndex, toIndex) &&
        fromIndex < items.size() && toIndex < items.size() && fromIndex != toIndex) {
        T movedItem = std::move(items[fromIndex]);
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(fromIndex));
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(toIndex), std::move(movedItem));
    }
}

/// @note 編集範囲は FluidInspector / Volume Flipbook Bake パネルと各ベイカーに揃える。
void ReflectGas(FluidGasSettings& gas, scene::IReflector& reflector)
{
    reflector.IntRange("resolution", gas.resolution, 0, 512);
    reflector.Tooltip("2D ベイクの格子の 1 辺。0 は Auto (コマの解像度に合わせる。上限 256)");
    reflector.FloatRange("buoyancy", gas.buoyancy, -10.0f, 20.0f);
    reflector.FloatRange("weight", gas.weight, 0.0f, 10.0f);
    reflector.FloatRange("vorticity", gas.vorticity, 0.0f, 4.0f);
    reflector.FloatRange("turbulence", gas.turbulence, 0.0f, 10.0f);
    reflector.FloatRange("turbulence_scale", gas.turbulenceScale, 0.1f, 32.0f);
    reflector.FloatRange("density_dissipation", gas.densityDissipation, 0.0f, 10.0f);
    reflector.FloatRange("temperature_dissipation", gas.temperatureDissipation, 0.0f, 20.0f);
    reflector.FloatRange("velocity_damping", gas.velocityDamping, 0.0f, 10.0f);
    reflector.FloatRange("ignition_temperature", gas.ignitionTemperature, 0.0f, 10.0f);
    reflector.FloatRange("burn_rate", gas.burnRate, 0.0f, 50.0f);
    reflector.FloatRange("burn_heat", gas.burnHeat, 0.0f, 20.0f);
    reflector.FloatRange("burn_smoke", gas.burnSmoke, 0.0f, 10.0f);
    reflector.FloatRange("burn_expansion", gas.burnExpansion, 0.0f, 20.0f);
    reflector.Field("wind", gas.wind);
    reflector.Field("floor", gas.floor);
    reflector.IntRange("pressure_iterations", gas.pressureIterations, 8, 200);
    reflector.Field("sharp_advection", gas.sharpAdvection);
    reflector.FloatRange("detail_period", gas.detailPeriod, 0.05f, 10.0f);
}

void ReflectLiquid(FluidLiquidSettings& liquid, scene::IReflector& reflector)
{
    reflector.IntRange("max_particles", liquid.maxParticles, 100, 20000);
    reflector.FloatRange("particle_radius", liquid.particleRadius, 0.004f, 0.05f);
    reflector.FloatRange("gravity", liquid.gravity, -20.0f, 40.0f);
    reflector.FloatRange("viscosity", liquid.viscosity, 0.0f, 1.0f);
    reflector.FloatRange("cohesion", liquid.cohesion, 0.0f, 1.0f);
    reflector.IntRange("solver_iterations", liquid.solverIterations, 1, 10);
    reflector.Field("floor", liquid.floor);
    reflector.FloatRange("floor_height", liquid.floorHeight, -1.5f, 1.0f);
    reflector.FloatRange("floor_friction", liquid.floorFriction, 0.0f, 2.0f);
    reflector.FloatRange("particle_lifetime", liquid.particleLifetime, 0.0f, 30.0f);
    reflector.Tooltip("0 は無限");
}

void ReflectMotion(FluidMotion& motion, scene::IReflector& reflector)
{
    reflector.BeginObject("motion");
    reflector.Field("inherit_velocity", motion.inheritVelocity);
    reflector.Tooltip("動く速さを流速 (気体) / 撃ち出す速度 (液体) に足す");
    ReflectList(reflector, "key", motion.keys, static_cast<std::size_t>(kMaxFluidMotionKeys), [&reflector](FluidMotionKey& key) {
        reflector.FloatRange("time", key.time, 0.0f, 60.0f);
        reflector.Tooltip("キーの間は直線でつなぐ。先頭より前・末尾より後は端のキーで止まる");
        reflector.Field("offset", key.offset);
        reflector.Tooltip("center からのずれ (正規化単位)");
    });
    reflector.EndObject();
}

void ReflectAmount(FluidAmount& amount, const char* scaleTooltip, scene::IReflector& reflector)
{
    reflector.BeginObject("amount");
    ReflectList(reflector, "key", amount.keys, static_cast<std::size_t>(kMaxFluidAmountKeys), [&](FluidAmountKey& key) {
        reflector.FloatRange("time", key.time, 0.0f, 60.0f);
        reflector.Tooltip("キーの間は直線でつなぐ。先頭より前・末尾より後は端のキーで止まる");
        reflector.FloatRange("scale", key.scale, 0.0f, 8.0f);
        reflector.Tooltip(scaleTooltip);
    });
    reflector.EndObject();
}

void ReflectSource(FluidSource& source, bool isLiquid, scene::IReflector& reflector)
{
    reflector.Field("enabled", source.enabled);
    reflector.Field("name", source.name);
    ReflectEnum(reflector, "shape", source.shape, labels::SOURCE_SHAPE);
    reflector.Tooltip("sphere: size.x = 半径 / box: size = 各軸の半分 / cone: 頂点が center、size.x = 底の半径、"
              "size.y = 長さ / ring: size.x = 輪の半径、size.y = 管の太さ / texture: 画像の濃さ (輝度 × α) の形に湧く板 / "
              "capsule: size.x = 半径、size.y = 芯の半分の長さ (両端は半球) / cylinder: size.x = 半径、size.y = 半分の高さ");
    reflector.Field("center", source.center);
    reflector.Tooltip("領域を各軸 [-1,1] に正規化した座標");
    reflector.Field("size", source.size);
    const bool isTextured = source.shape == FluidSourceShape::Texture;
    const bool isSegmentShape =
        source.shape == FluidSourceShape::Capsule || source.shape == FluidSourceShape::Cylinder;
    if (isTextured) reflector.Tooltip("size.x / size.y = 板の半幅 / 半高さ、size.z = 板の厚みの半分");
    else if (isSegmentShape) reflector.Tooltip("size.x = 半径、size.y = 軸方向の長さの半分 (size.z は見ない)");
    const bool hasDirection = source.shape == FluidSourceShape::Cone || source.shape == FluidSourceShape::Ring
        || isTextured || isSegmentShape;
    reflector.FieldIf("direction", source.direction, hasDirection,
              "cone の開く向き / ring・texture の法線 / capsule・cylinder の軸 (長さは問わない)。"
              "texture は (0,0,1) で画像が正面を向く");
    reflector.BeginField("texture", "texture");
    reflector.SetFieldVisible(isTextured);
    reflector.SetFileExtensions(".sprite,.png,.tga,.jpg,.jpeg");
    reflector.Field("texture", source.texture);
    if (isTextured) reflector.Tooltip("256×256 に縮めて使う。縦横比は size.x / size.y が持つ。"
                            "Sprite ならそのコマだけを切り抜く");
    reflector.EndField();
    FloatRangeIf(reflector, "density", source.density, 0.0f, 100.0f, !isLiquid, "1 秒あたりに足す量");
    FloatRangeIf(reflector, "temperature", source.temperature, 0.0f, 100.0f, !isLiquid);
    FloatRangeIf(reflector, "fuel", source.fuel, 0.0f, 100.0f, !isLiquid);
    FloatRangeIf(reflector, "noise", source.noise, 0.0f, 1.0f, !isLiquid, "注入量をノイズで揺らす");
    reflector.Field("velocity", source.velocity);
    reflector.Tooltip(isLiquid ? "撃ち出す速度 [領域単位/秒]" : "発生源の中の流速。0 なら流速には触らない");
    reflector.FloatRange("start_time", source.startTime, 0.0f, 60.0f);
    reflector.FloatRange("duration", source.duration, 0.0f, 60.0f);
    reflector.Tooltip(isLiquid ? "0 なら start_time に一斉に出す" : "0 は最後まで出し続ける");
    reflector.FloatRange("color_key", source.colorKey, 0.0f, 1.0f);
    reflector.Tooltip(isLiquid ? "render.albedo_ramp のどの色で描くか (粒子ごとに持つ)。use_albedo_ramp が false なら効かない"
                     : "render.albedo_ramp のどの色で描くか (煙に乗って運ばれ、混ざると色も混ざる)。"
                       "use_albedo_ramp が false なら効かない");
    FloatRangeIf(reflector, "spread", source.spread, 0.0f, 1.0f, isLiquid, "撃ち出す速度の大きさに対するばらつき");
    IntRangeIf(reflector, "count", source.count, 1, 20000, isLiquid, "撃ち出す総数");
    ReflectMotion(source.motion, reflector);
    ReflectAmount(source.amount,
                  isLiquid ? "density / temperature / fuel に掛かる倍率。キーが無ければ 1 "
                           "(液体はこの 3 つを使わないので効かない — 撒く量は count と duration が決める)"
                         : "density / temperature / fuel に掛かる倍率。キーが無ければ 1。"
                           "位置と撃ち出す速度には掛からない (動きは motion が持つ)",
                  reflector);
}

void ReflectForce(FluidForce& force, scene::IReflector& reflector)
{
    reflector.Field("enabled", force.enabled);
    reflector.Field("name", force.name);
    ReflectEnum(reflector, "type", force.type, labels::FORCE_TYPE);
    reflector.Field("center", force.center);
    reflector.Tooltip("領域を各軸 [-1,1] に正規化した座標");
    const bool hasDirection = force.type == FluidForceType::Wind || force.type == FluidForceType::Vortex;
    reflector.FieldIf("direction", force.direction, hasDirection, "wind: 向き / vortex: 回転軸 (2D では常に画面の奥行き軸)");
    /// @note 負を許すのは 2D の vortex の回り方を逆にする手段がこれしか無いため (軸は奥行きに固定)。
    reflector.FloatRange("strength", force.strength, -50.0f, 50.0f);
    reflector.Tooltip("加速度 [領域単位/秒²]。負で逆向き。drag は減衰係数 [1/秒] (0 以上)");
    reflector.FloatRange("radius", force.radius, 0.0f, 4.0f);
    reflector.Tooltip("影響半径。0 は領域全体に一様");
    FloatRangeIf(reflector, "falloff_power", force.falloffPower, 0.1f, 8.0f, force.radius > 0.0f,
                 "influence = (1 - 距離/radius)^falloff_power");
    const bool isNoise = force.type == FluidForceType::Noise;
    FloatRangeIf(reflector, "noise_frequency", force.noiseFrequency, 0.1f, 32.0f, isNoise, "領域幅あたりの山の数");
    FloatRangeIf(reflector, "noise_speed", force.noiseSpeed, 0.0f, 10.0f, isNoise, "ノイズが流れる速さ");
    reflector.FloatRange("start_time", force.startTime, 0.0f, 60.0f);
    reflector.FloatRange("duration", force.duration, 0.0f, 60.0f);
    reflector.Tooltip("0 はずっと効く");
    ReflectMotion(force.motion, reflector);
    ReflectAmount(force.amount, "strength に掛かる倍率。キーが無ければ 1", reflector);
}

void ReflectCollider(FluidCollider& collider, bool isLiquid, scene::IReflector& reflector)
{
    reflector.Field("enabled", collider.enabled);
    reflector.Field("name", collider.name);
    ReflectEnum(reflector, "shape", collider.shape, labels::COLLIDER_SHAPE);
    reflector.Tooltip("sphere: size.x = 半径 / box: size = 各軸の半分 (回転なし) / "
              "plane: center を通り direction を法線とする面。裏側がすべて固体 / "
              "capsule: size.x = 半径、size.y = 芯の半分の長さ (両端は半球) / cylinder: size.x = 半径、size.y = 半分の高さ");
    reflector.Field("center", collider.center);
    reflector.Tooltip("領域を各軸 [-1,1] に正規化した座標");
    const bool isPlane = collider.shape == FluidColliderShape::Plane;
    const bool isSegmentShape =
        collider.shape == FluidColliderShape::Capsule || collider.shape == FluidColliderShape::Cylinder;
    reflector.FieldIf("size", collider.size, !isPlane,
              isSegmentShape ? "size.x = 半径、size.y = 軸方向の長さの半分 (size.z は見ない)"
                      : "sphere: size.x = 半径 / box: 各軸の半分");
    reflector.FieldIf("direction", collider.direction, isPlane || isSegmentShape,
              isSegmentShape ? "軸の向き。長さは問わない (0 なら上向き)" : "面の法線。こちら側が流体 (長さは問わない)");
    FloatRangeIf(reflector, "friction", collider.friction, 0.0f, 2.0f, isLiquid, "表面に沿った速度を落とす強さ");
    reflector.FloatRange("start_time", collider.startTime, 0.0f, 60.0f);
    reflector.FloatRange("duration", collider.duration, 0.0f, 60.0f);
    reflector.Tooltip("0 はずっと居る");
    ReflectMotion(collider.motion, reflector);
}

void ReflectRamp(const char* name, FluidColorRamp& ramp, bool isVisible, const char* positionTooltip,
                 scene::IReflector& reflector)
{
    reflector.BeginField(name, name);
    reflector.SetFieldVisible(isVisible);
    /// @note 点の数は GPU / 3D の Ramp と同じ 4 で固定。足し引きさせない。
    reflector.SetFixedList(true);
    const std::size_t count = (std::min)(reflector.BeginObjectList(name, ramp.stops.size()), ramp.stops.size());
    /// @note 保存キーは «次の BeginField まで» 残る。戻さないと要素の color / position が Ramp の名で記録される。
    reflector.EndField();
    for (std::size_t index = 0; index < count; ++index) {
        FluidColorStop& stop = ramp.stops[index];
        reflector.BeginObjectElement(index);
        /// @note ColorField にしないのは、カラーピッカーが 1 を超える値 (HDR の芯) を丸めてしまうため。
        reflector.Field("color", stop.color);
        reflector.Tooltip("リニア (HDR 可)");
        reflector.FloatRange("position", stop.position, 0.0f, 1.0f);
        reflector.Tooltip(positionTooltip);
        reflector.EndObjectElement();
    }
    (void)reflector.EndObjectList();
}

void ReflectRender(FluidRenderSettings& renderSettings, bool isLiquid, scene::IReflector& reflector)
{
    ReflectEnum(reflector, "shading", renderSettings.shading, labels::SHADING);
    reflector.ColorField("smoke_color", renderSettings.smokeColor);
    reflector.ColorField("shadow_color", renderSettings.shadowColor);
    reflector.FloatRange("opacity", renderSettings.opacity, 0.0f, 50.0f);
    reflector.FloatRange("self_shadow", renderSettings.selfShadow, 0.0f, 20.0f);
    reflector.Field("light_direction", renderSettings.lightDirection);
    reflector.FloatRange("detail_strength", renderSettings.detailStrength, 0.0f, 1.0f);
    reflector.FloatRange("detail_scale", renderSettings.detailScale, 1.0f, 64.0f);
    reflector.FloatRange("fire_kelvin", renderSettings.fireKelvin, 500.0f, 6000.0f);
    reflector.FloatRange("fire_intensity", renderSettings.fireIntensity, 0.0f, 10.0f);
    reflector.Field("use_emission_ramp", renderSettings.useEmissionRamp);
    reflector.Tooltip("温度 (0〜1) → 発光の色を emission_ramp で決める。false なら fire は黒体、glow は smoke_color");
    ReflectRamp("emission_ramp", renderSettings.emissionRamp, renderSettings.useEmissionRamp,
                "温度 (0〜1) のどこに置くか。昇順に並べる", reflector);
    reflector.Field("use_albedo_ramp", renderSettings.useAlbedoRamp);
    reflector.Tooltip("発生源の color_key (0〜1) → 煙 / 液の地の色を albedo_ramp で決める。"
              "false なら smoke_color (glow では発光色) / liquid_color の 1 色");
    ReflectRamp("albedo_ramp", renderSettings.albedoRamp, renderSettings.useAlbedoRamp,
                "color_key (0〜1) のどこに置くか。昇順に並べる", reflector);
    reflector.ColorField("liquid_color", renderSettings.liquidColor);
    reflector.FloatRange("liquid_radius_scale", renderSettings.liquidRadiusScale, 0.5f, 6.0f);
    reflector.FloatRange("liquid_threshold", renderSettings.liquidThreshold, 0.05f, 2.0f);
    reflector.FloatRange("specular", renderSettings.specular, 0.0f, 2.0f);
    FloatRangeIf(reflector, "liquid_softness", renderSettings.liquidSoftness, 0.005f, 0.5f, isLiquid, "3D の液面: 縁の柔らかさ (密度の幅)");
    FloatRangeIf(reflector, "liquid_extinction", renderSettings.liquidExtinction, 0.0f, 200.0f, isLiquid,
                 "3D の液面: 濃さ。大きいほど不透明 (血)、小さいほど透ける (水)");
    FloatRangeIf(reflector, "liquid_gloss", renderSettings.liquidGloss, 1.0f, 512.0f, isLiquid, "3D の液面: 鏡面反射の鋭さ");
    FloatRangeIf(reflector, "liquid_fresnel", renderSettings.liquidFresnel, 0.0f, 0.2f, isLiquid, "3D の液面: 正面から見た反射率 (水 0.02)");
}

void ReflectOutput(FluidOutputSettings& output, scene::IReflector& reflector)
{
    reflector.IntRange("frame_size", output.frameSize, 16, 1024);
    reflector.IntRange("columns", output.columns, 1, 32);
    reflector.IntRange("rows", output.rows, 1, 32);
    reflector.IntRange("supersampling", output.supersampling, 1, 4);
    reflector.FloatRange("duration", output.duration, 0.05f, 30.0f);
    reflector.FloatRange("warmup", output.warmup, 0.0f, 30.0f);
    reflector.IntRange("substeps", output.substeps, 1, 16);
    reflector.Field("loop", output.loop);
    reflector.FloatRange("loop_blend_fraction", output.loopBlendFraction, 0.0f, 0.5f);
    reflector.Field("motion_vectors", output.motionVectors);
    reflector.Field("vector_field", output.vectorField);
    reflector.IntRange("vector_field_resolution", output.vectorFieldResolution, 8, 64);
    reflector.Field("vector_field_extents", output.vectorFieldExtents);
}

void ReflectBake(FluidBakeSettings& bake, scene::IReflector& reflector)
{
    ReflectEnum(reflector, "mode", bake.mode, labels::BAKE_MODE);
    reflector.Tooltip("2d は .fluid の Bake (2D ソルバー)、3d は Volume Flipbook Baker (3D で解いてレイマーチ)");
    reflector.IntRange("volume_resolution", bake.volumeResolution, 16, 160);
    reflector.Tooltip("3D の格子の 1 辺。GPU は 160、CPU は 96 まで");
    reflector.IntRange("ray_steps", bake.raySteps, 32, 512);
    reflector.Tooltip("視線 1 本の標本数。volume_resolution の 2 倍が目安 (据え置きだと格子を上げても絵が変わらない)");
    reflector.IntRange("shadow_steps", bake.shadowSteps, 4, 64);
    reflector.Tooltip("影の行進の標本数。増やすと影の縞が減るが、焼き時間は ray_steps との積で増える");
    ReflectEnum(reflector, "solver", bake.solver, labels::BAKE_SOLVER);
    reflector.Tooltip("3D で解くソルバー。液体の gpu は GPU の粒子ソルバー (新しく検証中。液体のプリセットは cpu)");
    reflector.FloatRange("density_scale", bake.densityScale, 0.01f, 20.0f);
    reflector.IntRange("scattering_octaves", bake.scatteringOctaves, 1, 8);
    reflector.FloatRange("sky_occlusion", bake.skyOcclusion, 0.0f, 1.0f);
    reflector.Field("blackbody_emission", bake.blackbodyEmission);
    reflector.FloatRange("blackbody_min_kelvin", bake.blackbodyMinKelvin, 500.0f, 15000.0f);
    reflector.FloatRange("blackbody_max_kelvin", bake.blackbodyMaxKelvin, 500.0f, 15000.0f);
    reflector.Field("six_way_lightmaps", bake.sixWayLightmaps);
    reflector.FloatRange("light_yaw_degrees", bake.lightYawDegrees, -180.0f, 180.0f);
    reflector.FloatRange("light_pitch_degrees", bake.lightPitchDegrees, -10.0f, 90.0f);
    reflector.ColorField("light_color", bake.lightColor);
    reflector.ColorField("ambient", bake.ambient);
    reflector.FloatRange("extinction", bake.extinction, 0.0f, 100.0f);
    reflector.FloatRange("anisotropy", bake.anisotropy, -0.95f, 0.95f);
    reflector.FloatRange("emission_intensity", bake.emissionIntensity, 0.0f, 100.0f);
    reflector.FloatRange("exposure", bake.exposure, 0.01f, 8.0f);
    reflector.FloatRange("camera_yaw_degrees", bake.cameraYawDegrees, -180.0f, 180.0f);
    reflector.FloatRange("half_extent", bake.halfExtent, 0.05f, 4.0f);
}

} /// @note namespace

void ReflectFluidRecipe(FluidRecipe& recipe, scene::IReflector& reflector)
{
    reflector.Group("Recipe");
    ReflectEnum(reflector, "kind", recipe.kind, labels::KIND);
    /// @note IReflector に符号なしの Field は無い。読むだけのリフレクタで値を壊さないよう、変わったときだけ書き戻す。
    int seed = static_cast<int>(recipe.seed);
    reflector.Field("seed", seed);
    if (seed != static_cast<int>(recipe.seed)) recipe.seed = static_cast<uint32_t>((std::max)(seed, 0));
    /// @note kind の読み替えで意味の変わる項目 (気体の注入量 / 液体の撃ち出し数) だけを出し分ける。
    const bool isLiquid = recipe.kind == FluidKind::Liquid;

    reflector.Group("Gas");
    reflector.BeginObject("gas");
    ReflectGas(recipe.gas, reflector);
    reflector.EndObject();

    reflector.Group("Liquid");
    reflector.BeginObject("liquid");
    ReflectLiquid(recipe.liquid, reflector);
    reflector.EndObject();

    reflector.Group("Sources");
    ReflectList(reflector, "source", recipe.sources, static_cast<std::size_t>(kMaxFluidSources),
                [&reflector, isLiquid](FluidSource& source) { ReflectSource(source, isLiquid, reflector); });

    reflector.Group("Forces");
    ReflectList(reflector, "force", recipe.forces, static_cast<std::size_t>(kMaxFluidForces),
                [&reflector](FluidForce& force) { ReflectForce(force, reflector); });

    reflector.Group("Colliders");
    ReflectList(reflector, "collider", recipe.colliders, static_cast<std::size_t>(kMaxFluidColliders),
                [&reflector, isLiquid](FluidCollider& collider) { ReflectCollider(collider, isLiquid, reflector); });

    reflector.Group("Render");
    reflector.BeginObject("render");
    ReflectRender(recipe.render, isLiquid, reflector);
    reflector.EndObject();

    reflector.Group("Output");
    reflector.BeginObject("output");
    ReflectOutput(recipe.output, reflector);
    reflector.EndObject();

    reflector.Group("Bake");
    reflector.BeginObject("bake");
    ReflectBake(recipe.bake, reflector);
    reflector.EndObject();
}

} /// @note namespace fbzz::asset
