// FBZZ Engine
// VFXAuthoringSchema.hpp | fbzz::asset
// VFXノード設定をInspector・binding・AIで共有するauthoringスキーマ
#pragma once

#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Reflection/TypeSchema.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::asset {
namespace detail {

class StaticTypeSchema final : public reflection::ITypeSchema {
public:
    StaticTypeSchema(std::string_view name, std::vector<reflection::PropertyDesc> properties)
        : m_name(name), m_properties(std::move(properties)) {}
    [[nodiscard]] std::string_view TypeName() const override { return m_name; }
    [[nodiscard]] std::span<const reflection::PropertyDesc> Properties() const override {
        return m_properties;
    }
private:
    std::string_view m_name;
    std::vector<reflection::PropertyDesc> m_properties;
};

inline reflection::RangeHint Range(float minimum, float maximum) {
    return { true, minimum, maximum };
}

// enum の値名 (index 順)。Inspector の Combo と AI の vfx.schema が同じ表を読む。
// ScriptParticleProxy.hpp の enum 定義と順序・個数を一致させること。
inline constexpr std::string_view kShapeNames[] = {
    "Point", "Sphere", "Cone", "Box", "Mesh Surface" };
inline constexpr std::string_view kBlendModeNames[] = { "Additive", "Alpha", "Premultiplied" };
inline constexpr std::string_view kSortModeNames[] = { "None", "Back To Front" };
inline constexpr std::string_view kSimulationModeNames[] = { "CPU", "GPU" };
inline constexpr std::string_view kSimulationSpaceNames[] = { "World", "Local" };
inline constexpr std::string_view kRenderModeNames[] = {
    "Billboard", "Stretched Billboard", "Horizontal Billboard", "Vertical Billboard" };
inline constexpr std::string_view kCollisionModeNames[] = { "None", "Physics", "Plane", "Depth" };
inline constexpr std::string_view kCollisionResponseNames[] = { "Bounce", "Kill", "Stop" };
// Rendering/Mask.hlsli の FBZZ_MASK_* と同じ並び。
inline constexpr std::string_view kAlphaSourceNames[] = {
    "Texture Alpha", "Luminance (black = clear)", "Inverted Luminance (white = clear)",
    "Red Channel", "Green Channel", "Blue Channel", "Inverted Alpha" };
inline constexpr std::string_view kFlipbookModeNames[] = {
    "Lifetime", "Frames Per Second", "Random Frame", "Ping Pong" };

} // namespace detail

// Burst 1 件分。ParticleEmitter::bursts の要素型スキーマとして使う。
// "particle.bursts[0].count" のような添字付き schemaPath で Inspector / AI から到達する。
[[nodiscard]] inline const reflection::ITypeSchema& GetParticleBurstSchema()
{
    using B = scene::ParticleBurst;
    using reflection::MakeProperty;
    using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("ParticleBurst", {
        MakeProperty<B, float, &B::time>("time", PropertyType::Float, "Time", "Burst", true, detail::Range(0, 3600)),
        MakeProperty<B, int, &B::count>("count", PropertyType::Int, "Count", "Burst", true, detail::Range(0, 1000000)),
        MakeProperty<B, int, &B::cycles>("cycles", PropertyType::Int, "Cycles", "Burst", true, detail::Range(1, 100000)),
        MakeProperty<B, float, &B::interval>("interval", PropertyType::Float, "Interval", "Burst", true, detail::Range(0, 3600)),
        MakeProperty<B, float, &B::probability>("probability", PropertyType::Float, "Probability", "Burst", true, detail::Range(0, 1)),
    });
    return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetParticleEmitterSchema()
{
    using P = scene::ParticleEmitter;
    using reflection::MakeArrayProperty;
    using reflection::MakeEnumProperty;
    using reflection::MakeIntProperty;
    using reflection::MakeProperty;
    using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("ParticleEmitter", {
        MakeProperty<P, math::Vector3, &P::emitPosition>("emitPosition", PropertyType::Vector3, "Emit Position", "Emission", true),
        MakeProperty<P, math::Vector3, &P::emitVelocity>("emitVelocity", PropertyType::Vector3, "Emit Velocity", "Emission", true),
        MakeProperty<P, float, &P::velocitySpread>("velocitySpread", PropertyType::Float, "Velocity Spread", "Emission", true, detail::Range(0, 100)),
        MakeProperty<P, math::Vector4, &P::colorStart>("colorStart", PropertyType::Color, "Start Color", "Color", true),
        MakeProperty<P, math::Vector4, &P::colorEnd>("colorEnd", PropertyType::Color, "End Color", "Color", true),
        MakeProperty<P, float, &P::colorVariation>("colorVariation", PropertyType::Float, "Color Variation", "Color", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::sizeStart>("sizeStart", PropertyType::Float, "Start Size", "Main", true, detail::Range(0, 1000)),
        MakeProperty<P, float, &P::sizeEnd>("sizeEnd", PropertyType::Float, "End Size", "Main", true, detail::Range(0, 1000)),
        MakeProperty<P, math::Vector3, &P::sizeAxisScale>("sizeAxisScale", PropertyType::Vector3, "Size Axis Scale", "Main", true),
        MakeProperty<P, float, &P::lifetime>("lifetime", PropertyType::Float, "Lifetime", "Main", true, detail::Range(0.001f, 3600)),
        MakeProperty<P, float, &P::lifetimeRandom>("lifetimeRandom", PropertyType::Float, "Lifetime Random", "Main", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::emitRate>("emitRate", PropertyType::Float, "Rate", "Emission", true, detail::Range(0, 1000000)),
        MakeProperty<P, int, &P::maxParticles>("maxParticles", PropertyType::Int, "Max Particles", "Main", true, detail::Range(1, 10000000)),
        MakeProperty<P, math::Vector3, &P::gravity>("gravity", PropertyType::Vector3, "Gravity", "Forces", true),
        MakeProperty<P, bool, &P::loop>("loop", PropertyType::Bool, "Loop", "Main", true),
        MakeProperty<P, float, &P::duration>("duration", PropertyType::Float, "Duration", "Main", true, detail::Range(0, 3600)),
        MakeProperty<P, float, &P::startDelay>("startDelay", PropertyType::Float, "Start Delay", "Main", true, detail::Range(0, 3600)),
        MakeEnumProperty<P, scene::ParticleEmitterShape, &P::shape, 4>("shape", "Shape", "Shape", detail::kShapeNames),
        MakeProperty<P, float, &P::sphereRadius>("sphereRadius", PropertyType::Float, "Sphere Radius", "Shape", true, detail::Range(0, 10000)),
        MakeProperty<P, float, &P::coneAngleDegrees>("coneAngleDegrees", PropertyType::Float, "Cone Angle", "Shape", true, detail::Range(0, 180)),
        MakeProperty<P, float, &P::coneRadius>("coneRadius", PropertyType::Float, "Cone Radius", "Shape", true, detail::Range(0, 10000)),
        MakeProperty<P, math::Vector3, &P::boxExtents>("boxExtents", PropertyType::Vector3, "Box Extents", "Shape", true),
        MakeProperty<P, std::string, &P::meshShapePath>("meshShapePath", PropertyType::AssetRef, "Mesh Shape", "Shape", true),
        MakeProperty<P, float, &P::meshShapeScale>("meshShapeScale", PropertyType::Float, "Mesh Scale", "Shape", true, detail::Range(0.001f, 1000)),
        MakeEnumProperty<P, scene::ParticleSimulationMode, &P::simulationMode, 1>("simulationMode", "Simulation", "Renderer", detail::kSimulationModeNames),
        MakeEnumProperty<P, scene::ParticleSimulationSpace, &P::simulationSpace, 1>("simulationSpace", "Simulation Space", "Renderer", detail::kSimulationSpaceNames),
        MakeEnumProperty<P, scene::ParticleRenderMode, &P::renderMode, 3>("renderMode", "Render Mode", "Renderer", detail::kRenderModeNames),
        MakeEnumProperty<P, scene::ParticleBlendMode, &P::blendMode, 2>("blendMode", "Blend Mode", "Renderer", detail::kBlendModeNames),
        MakeIntProperty<P, int, &P::renderPriority>("renderPriority", "Render Priority", "Renderer", true, detail::Range(-1000, 1000)),
        MakeEnumProperty<P, scene::ParticleSortMode, &P::sortMode, 1>("sortMode", "Sort Mode", "Renderer", detail::kSortModeNames),
        MakeEnumProperty<P, scene::ParticleAlphaSource, &P::alphaSource, 6>("alphaSource", "Alpha Source", "Renderer", detail::kAlphaSourceNames),
        MakeProperty<P, float, &P::stretchedVelocityScale>("stretchedVelocityScale", PropertyType::Float, "Velocity Scale", "Renderer", true),
        MakeProperty<P, float, &P::stretchedLengthScale>("stretchedLengthScale", PropertyType::Float, "Length Scale", "Renderer", true),
        MakeProperty<P, std::string, &P::materialPath>("materialPath", PropertyType::AssetRef, "Material", "Renderer", true),
        MakeProperty<P, std::string, &P::meshParticlePath>("meshParticlePath", PropertyType::AssetRef, "Mesh Particle", "Renderer", true),
        MakeProperty<P, int, &P::spriteColumns>("spriteColumns", PropertyType::Int, "Columns", "Flipbook", true, detail::Range(1, 256)),
        MakeProperty<P, int, &P::spriteRows>("spriteRows", PropertyType::Int, "Rows", "Flipbook", true, detail::Range(1, 256)),
        MakeProperty<P, float, &P::flipbookFramesPerSecond>("flipbookFramesPerSecond", PropertyType::Float, "FPS", "Flipbook", true),
        MakeProperty<P, bool, &P::flipbookFrameBlending>("flipbookFrameBlending", PropertyType::Bool, "Frame Blending", "Flipbook", true),
        MakeProperty<P, bool, &P::spriteRandomStartFrame>("spriteRandomStartFrame", PropertyType::Bool, "Random Start Frame", "Flipbook", true),
        MakeProperty<P, bool, &P::spriteRandomRow>("spriteRandomRow", PropertyType::Bool, "Random Row", "Flipbook", true),
        MakeProperty<P, bool, &P::motionVectorFlipbook>("motionVectorFlipbook", PropertyType::Bool, "Motion Vector Blend", "Flipbook", true),
        MakeProperty<P, std::string, &P::motionVectorTexturePath>("motionVectorTexturePath", PropertyType::AssetRef, "Motion Vector Atlas", "Flipbook", true),
        MakeProperty<P, float, &P::motionVectorStrength>("motionVectorStrength", PropertyType::Float, "Motion Strength", "Flipbook", true, detail::Range(0, 8)),
        MakeProperty<P, float, &P::velocityDamping>("velocityDamping", PropertyType::Float, "Velocity Damping", "Forces", true),
        MakeProperty<P, float, &P::angularVelocityMin>("angularVelocityMin", PropertyType::Float, "Angular Min", "Rotation", true),
        MakeProperty<P, float, &P::angularVelocityMax>("angularVelocityMax", PropertyType::Float, "Angular Max", "Rotation", true),
        MakeProperty<P, bool, &P::useSizeCurve>("useSizeCurve", PropertyType::Bool, "Use Size Curve", "Curves", true),
        MakeProperty<P, scene::ParticleCurve, &P::sizeCurve>("sizeCurve", PropertyType::Curve, "Size Curve", "Curves", true),
        MakeProperty<P, bool, &P::useVelocityCurve>("useVelocityCurve", PropertyType::Bool, "Use Velocity Curve", "Curves", true),
        MakeProperty<P, scene::ParticleCurve, &P::velocityCurve>("velocityCurve", PropertyType::Curve, "Velocity Curve", "Curves", true),
        MakeProperty<P, bool, &P::useColorGradient>("useColorGradient", PropertyType::Bool, "Use Color Gradient", "Curves", true),
        MakeProperty<P, scene::ParticleGradient, &P::colorGradient>("colorGradient", PropertyType::Gradient, "Color Gradient", "Curves", true),
        MakeProperty<P, bool, &P::useRotationCurve>("useRotationCurve", PropertyType::Bool, "Use Rotation Curve", "Curves", true),
        MakeProperty<P, scene::ParticleCurve, &P::rotationCurve>("rotationCurve", PropertyType::Curve, "Rotation Curve", "Curves", true, detail::Range(0, 2)),
        MakeProperty<P, bool, &P::useDragCurve>("useDragCurve", PropertyType::Bool, "Use Drag Curve", "Curves", true),
        MakeProperty<P, scene::ParticleCurve, &P::dragCurve>("dragCurve", PropertyType::Curve, "Drag Curve", "Curves", true, detail::Range(0, 4)),
        MakeProperty<P, math::Vector3, &P::orbitalAxis>("orbitalAxis", PropertyType::Vector3, "Orbital Axis", "Velocity", true),
        MakeProperty<P, float, &P::orbitalVelocity>("orbitalVelocity", PropertyType::Float, "Orbital Velocity", "Velocity", true, detail::Range(-100, 100)),
        MakeProperty<P, float, &P::radialVelocity>("radialVelocity", PropertyType::Float, "Radial Velocity", "Velocity", true, detail::Range(-100, 100)),
        MakeProperty<P, float, &P::inheritVelocity>("inheritVelocity", PropertyType::Float, "Inherit Velocity", "Velocity", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::rateOverDistance>("rateOverDistance", PropertyType::Float, "Rate Over Distance", "Emission", true),
        MakeProperty<P, bool, &P::prewarm>("prewarm", PropertyType::Bool, "Prewarm", "Emission", true),
        MakeProperty<P, bool, &P::trailEnabled>("trailEnabled", PropertyType::Bool, "Trail", "Trail", true),
        MakeIntProperty<P, int, &P::trailPointCount>("trailPointCount", "Trail Points", "Trail", true, detail::Range(1, 8)),
        MakeProperty<P, float, &P::trailSampleInterval>("trailSampleInterval", PropertyType::Float, "Sample Interval", "Trail", true, detail::Range(0.001f, 1)),
        MakeProperty<P, float, &P::trailWidthScale>("trailWidthScale", PropertyType::Float, "Tip Width", "Trail", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::trailAlphaScale>("trailAlphaScale", PropertyType::Float, "Tip Alpha", "Trail", true, detail::Range(0, 1)),
        MakeProperty<P, math::Vector4, &P::trailColorTint>("trailColorTint", PropertyType::Color, "Trail Tint", "Trail", true),
        MakeProperty<P, bool, &P::trailRibbon>("trailRibbon", PropertyType::Bool, "Continuous Ribbon", "Trail", true),
        MakeProperty<P, float, &P::trailRibbonWidth>("trailRibbonWidth", PropertyType::Float, "Ribbon Width", "Trail", true, detail::Range(0, 20)),
        MakeProperty<P, float, &P::selfShadowStrength>("selfShadowStrength", PropertyType::Float, "Self Shadow", "Renderer", true, detail::Range(0, 8)),
        MakeProperty<P, bool, &P::softParticles>("softParticles", PropertyType::Bool, "Soft Particles", "Renderer", true),
        MakeProperty<P, float, &P::softParticleFadeDistance>("softParticleFadeDistance", PropertyType::Float, "Soft Fade", "Renderer", true),
        MakeProperty<P, bool, &P::distortion>("distortion", PropertyType::Bool, "Distortion", "Renderer", true),
        MakeProperty<P, float, &P::distortionStrength>("distortionStrength", PropertyType::Float, "Distortion Strength", "Renderer", true, detail::Range(0, 0.25f)),
        MakeProperty<P, bool, &P::sixWayLighting>("sixWayLighting", PropertyType::Bool, "Six-way Lit Smoke", "Renderer", true),
        // WHY: Particle.hlsl は lerp(1, lit, saturate(gLightingStrength)) で使うため 1.0 超は
        //      「元の色を完全に (ambient + N·L) へ置換」の意味しか持たない。暗い環境では
        //      煙が真っ黒に潰れるだけなので、レンジ自体を効果のある範囲へ絞る。
        MakeProperty<P, float, &P::lightingStrength>("lightingStrength", PropertyType::Float, "Lighting Strength", "Renderer", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::emissiveScale>("emissiveScale", PropertyType::Float, "HDR Emissive", "Renderer", true, detail::Range(0, 100)),
        MakeProperty<P, bool, &P::receiveShadows>("receiveShadows", PropertyType::Bool, "Receive Shadows", "Renderer", true),
        MakeProperty<P, float, &P::shadowStrength>("shadowStrength", PropertyType::Float, "Shadow Strength", "Renderer", true, detail::Range(0, 1)),
        MakeProperty<P, bool, &P::volumetric>("volumetric", PropertyType::Bool, "Volumetric Smoke", "Volumetric", true),
        MakeIntProperty<P, int, &P::volumetricSteps>("volumetricSteps", "Steps", "Volumetric", true, detail::Range(1, 64)),
        MakeProperty<P, float, &P::volumetricDensity>("volumetricDensity", PropertyType::Float, "Density", "Volumetric", true, detail::Range(0, 20)),
        MakeProperty<P, float, &P::volumetricAnisotropy>("volumetricAnisotropy", PropertyType::Float, "Anisotropy", "Volumetric", true, detail::Range(-0.95f, 0.95f)),
        MakeProperty<P, float, &P::volumetricNoiseScale>("volumetricNoiseScale", PropertyType::Float, "Noise Scale", "Volumetric", true, detail::Range(0, 32)),
        MakeProperty<P, bool, &P::lodEnabled>("lodEnabled", PropertyType::Bool, "LOD", "LOD", true),
        MakeProperty<P, float, &P::lodNearDistance>("lodNearDistance", PropertyType::Float, "Near Distance", "LOD", true),
        MakeProperty<P, float, &P::lodFarDistance>("lodFarDistance", PropertyType::Float, "Far Distance", "LOD", true),
        MakeProperty<P, float, &P::lodFarRateScale>("lodFarRateScale", PropertyType::Float, "Far Rate", "LOD", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::noiseStrength>("noiseStrength", PropertyType::Float, "Strength", "Noise", true),
        MakeProperty<P, float, &P::noiseFrequency>("noiseFrequency", PropertyType::Float, "Frequency", "Noise", true),
        MakeProperty<P, float, &P::noiseSpeed>("noiseSpeed", PropertyType::Float, "Speed", "Noise", true),

        // ── ここから下は「実フィールドはあるのにスキーマへ載っていなかった」分の補完 ──
        // WHY: スキーマ leaf の集合が authoring フィールドの集合と一致していないと、
        //      binding 候補にも AI の vfx.schema にも現れず、設計の「1定義→N生成ビュー」が崩れる。
        //      Projects/Tests/VFXSchema が両者の一致を検証しているため、以後の追加漏れは落ちる。
        MakeProperty<P, bool, &P::clearOnStop>("clearOnStop", PropertyType::Bool, "Clear On Stop", "Main", true),
        MakeIntProperty<P, std::uint32_t, &P::randomSeed>("randomSeed", "Random Seed", "Main", true, detail::Range(1, 2147483647)),
        MakeIntProperty<P, int, &P::meshShapeIndex>("meshShapeIndex", "Mesh Submesh Index", "Shape", true, detail::Range(-1, 4096)),
        MakeProperty<P, bool, &P::meshShapeFollowSkinnedAnimation>("meshShapeFollowSkinnedAnimation", PropertyType::Bool, "Follow Skinned Animation", "Shape", true),

        MakeEnumProperty<P, scene::ParticleFlipbookMode, &P::flipbookMode, 3>("flipbookMode", "Flipbook Mode", "Flipbook", detail::kFlipbookModeNames),
        MakeIntProperty<P, int, &P::spriteStartFrame>("spriteStartFrame", "Start Frame", "Flipbook", true, detail::Range(0, 65536)),
        MakeIntProperty<P, int, &P::spriteEndFrame>("spriteEndFrame", "End Frame", "Flipbook", true, detail::Range(0, 65536)),

        MakeEnumProperty<P, scene::ParticleCollisionMode, &P::collisionMode, 3>("collisionMode", "Collision Mode", "Collision", detail::kCollisionModeNames),
        MakeEnumProperty<P, scene::ParticleCollisionResponse, &P::collisionResponse, 2>("collisionResponse", "Collision Response", "Collision", detail::kCollisionResponseNames),
        MakeProperty<P, float, &P::collisionRadius>("collisionRadius", PropertyType::Float, "Radius", "Collision", true, detail::Range(0, 100)),
        MakeProperty<P, float, &P::collisionBounciness>("collisionBounciness", PropertyType::Float, "Bounciness", "Collision", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::collisionDamping>("collisionDamping", PropertyType::Float, "Damping", "Collision", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::collisionPlaneY>("collisionPlaneY", PropertyType::Float, "Plane Y", "Collision", true),

        MakeProperty<P, std::string, &P::birthSubEmitter>("birthSubEmitter", PropertyType::String, "On Birth", "Sub Emitter", true),
        MakeProperty<P, std::string, &P::deathSubEmitter>("deathSubEmitter", PropertyType::String, "On Death", "Sub Emitter", true),
        MakeProperty<P, std::string, &P::collisionSubEmitter>("collisionSubEmitter", PropertyType::String, "On Collision", "Sub Emitter", true),
        MakeIntProperty<P, int, &P::subEmitterBurstCount>("subEmitterBurstCount", "Burst Count", "Sub Emitter", true, detail::Range(0, 100000)),

        MakeProperty<P, float, &P::sizeCurvePower>("sizeCurvePower", PropertyType::Float, "Size Curve Power", "Curves", true, detail::Range(0.001f, 16)),
        MakeProperty<P, float, &P::colorCurvePower>("colorCurvePower", PropertyType::Float, "Color Curve Power", "Curves", true, detail::Range(0.001f, 16)),

        MakeProperty<P, bool, &P::cullingEnabled>("cullingEnabled", PropertyType::Bool, "Culling", "LOD", true),
        MakeProperty<P, float, &P::cullingBoundsPadding>("cullingBoundsPadding", PropertyType::Float, "Bounds Padding", "LOD", true, detail::Range(0, 100)),
        MakeProperty<P, float, &P::lodNearRateScale>("lodNearRateScale", PropertyType::Float, "Near Rate", "LOD", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::screenCoverageThreshold>("screenCoverageThreshold", PropertyType::Float, "Screen Coverage Threshold", "LOD", true, detail::Range(0, 1)),
        MakeProperty<P, bool, &P::pauseWhenCulled>("pauseWhenCulled", PropertyType::Bool, "Pause When Culled", "LOD", true),
        MakeProperty<P, bool, &P::receiveForceFields>("receiveForceFields", PropertyType::Bool, "Receive Force Fields", "Forces", true),

        MakeArrayProperty<P, std::vector<scene::ParticleBurst>, &P::bursts>("bursts", "Bursts", "Emission", GetParticleBurstSchema(), true),
    });
    return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXTrailSchema()
{
    using T = VFXTrailSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXTrailSettings", {
        MakeProperty<T, std::string, &T::meshPath>("meshPath", PropertyType::AssetRef, "Mesh", "Asset", true),
        MakeProperty<T, std::string, &T::materialPath>("materialPath", PropertyType::AssetRef, "Material", "Asset", true),
        MakeProperty<T, math::Vector4, &T::colorStart>("colorStart", PropertyType::Color, "Start Color", "Color", true),
        MakeProperty<T, math::Vector4, &T::colorEnd>("colorEnd", PropertyType::Color, "End Color", "Color", true),
        MakeProperty<T, float, &T::lifetime>("lifetime", PropertyType::Float, "Lifetime", "Main", true),
        MakeProperty<T, float, &T::widthStart>("widthStart", PropertyType::Float, "Start Width", "Main", true),
        MakeProperty<T, float, &T::widthEnd>("widthEnd", PropertyType::Float, "End Width", "Main", true),
        MakeProperty<T, bool, &T::beamMode>("beamMode", PropertyType::Bool, "Beam Mode", "Beam", true),
        MakeProperty<T, math::Vector3, &T::beamStart>("beamStart", PropertyType::Vector3, "Start", "Beam", true),
        MakeProperty<T, math::Vector3, &T::beamEnd>("beamEnd", PropertyType::Vector3, "End", "Beam", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXLightSchema()
{
    using T = VFXLightSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXLightSettings", {
        MakeProperty<T, math::Vector3, &T::color>("color", PropertyType::Vector3, "Color", "Light", true),
        MakeProperty<T, float, &T::intensity>("intensity", PropertyType::Float, "Intensity", "Light", true),
        MakeProperty<T, float, &T::range>("range", PropertyType::Float, "Range", "Light", true),
        MakeProperty<T, bool, &T::useIntensityCurve>("useIntensityCurve", PropertyType::Bool, "Use Intensity Curve", "Light", true),
        MakeProperty<T, scene::ParticleCurve, &T::intensityCurve>("intensityCurve", PropertyType::Curve, "Intensity Curve", "Light", true, detail::Range(0, 1)),
        MakeProperty<T, bool, &T::useColorGradient>("useColorGradient", PropertyType::Bool, "Use Color Gradient", "Light", true),
        MakeProperty<T, scene::ParticleGradient, &T::colorGradient>("colorGradient", PropertyType::Gradient, "Color Gradient", "Light", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXAudioSchema()
{
    using T = VFXAudioSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXAudioSettings", {
        MakeProperty<T, std::string, &T::clipPath>("clipPath", PropertyType::AssetRef, "Clip", "Audio", true),
        MakeProperty<T, float, &T::volume>("volume", PropertyType::Float, "Volume", "Audio", true, detail::Range(0, 1)),
        MakeProperty<T, float, &T::pitch>("pitch", PropertyType::Float, "Pitch", "Audio", true),
        MakeProperty<T, float, &T::spatialBlend>("spatialBlend", PropertyType::Float, "Spatial Blend", "Audio", true, detail::Range(0, 1)),
        MakeProperty<T, bool, &T::loop>("loop", PropertyType::Bool, "Loop", "Audio", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXDecalSchema()
{
    using T = VFXDecalSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXDecalSettings", {
        MakeProperty<T, std::string, &T::materialPath>("materialPath", PropertyType::AssetRef, "Material", "Decal", true),
        MakeProperty<T, std::string, &T::albedoPath>("albedoPath", PropertyType::AssetRef, "Albedo", "Decal", true),
        MakeProperty<T, std::string, &T::normalPath>("normalPath", PropertyType::AssetRef, "Normal", "Decal", true),
        MakeProperty<T, std::string, &T::emissivePath>("emissivePath", PropertyType::AssetRef, "Emissive", "Decal", true),
        MakeProperty<T, math::Vector4, &T::color>("color", PropertyType::Color, "Color", "Decal", true),
        MakeProperty<T, float, &T::normalStrength>("normalStrength", PropertyType::Float, "Normal Strength", "Decal", true),
        MakeProperty<T, float, &T::emissiveScale>("emissiveScale", PropertyType::Float, "Emissive Scale", "Decal", true),
        MakeProperty<T, float, &T::fadeTime>("fadeTime", PropertyType::Float, "Fade Time", "Decal", true),
        MakeProperty<T, bool, &T::useFadeCurve>("useFadeCurve", PropertyType::Bool, "Use Fade Curve", "Decal", true),
        MakeProperty<T, scene::ParticleCurve, &T::fadeCurve>("fadeCurve", PropertyType::Curve, "Fade Curve", "Decal", true, detail::Range(0, 1)),
        MakeProperty<T, float, &T::angleFadeStrength>("angleFadeStrength", PropertyType::Float, "Angle Fade", "Decal", true, detail::Range(0, 1)),
        MakeProperty<T, float, &T::angleFadeDegrees>("angleFadeDegrees", PropertyType::Float, "Angle Fade Limit", "Decal", true, detail::Range(0, 89)),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXSubGraphSchema()
{
    using T = VFXSubGraphSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXSubGraphSettings", {
        MakeProperty<T, std::string, &T::graphPath>("graphPath", PropertyType::AssetRef, "Graph", "Sub Graph", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXForceFieldSchema()
{
    using T = VFXForceFieldSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXForceFieldSettings", {
        MakeProperty<T, int, &T::fieldType>("fieldType", PropertyType::Int, "Field Type", "Force Field", true, detail::Range(0, 5)),
        MakeProperty<T, float, &T::strength>("strength", PropertyType::Float, "Strength", "Force Field", true),
        MakeProperty<T, float, &T::radius>("radius", PropertyType::Float, "Radius", "Force Field", true),
        MakeProperty<T, float, &T::falloffPower>("falloffPower", PropertyType::Float, "Falloff Power", "Force Field", true, detail::Range(0, 16)),
        MakeProperty<T, math::Vector3, &T::direction>("direction", PropertyType::Vector3, "Direction / Axis", "Force Field", true),
        MakeProperty<T, float, &T::noiseFrequency>("noiseFrequency", PropertyType::Float, "Noise Frequency", "Force Field", true),
        MakeProperty<T, float, &T::noiseSpeed>("noiseSpeed", PropertyType::Float, "Noise Speed", "Force Field", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXMeshSchema()
{
    using T = VFXMeshSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXMeshSettings", {
        MakeProperty<T, std::string, &T::meshPath>("meshPath", PropertyType::AssetRef, "Mesh", "Mesh", true),
        MakeProperty<T, std::string, &T::materialPath>("materialPath", PropertyType::AssetRef, "Material", "Mesh", true),
        MakeProperty<T, float, &T::scaleStart>("scaleStart", PropertyType::Float, "Scale Start", "Mesh", true),
        MakeProperty<T, float, &T::scaleEnd>("scaleEnd", PropertyType::Float, "Scale End", "Mesh", true),
        MakeProperty<T, float, &T::scaleEasePower>("scaleEasePower", PropertyType::Float, "Ease Power", "Mesh", true, detail::Range(0.05f, 4)),
        MakeProperty<T, math::Vector4, &T::colorStart>("colorStart", PropertyType::Color, "Color Start", "Mesh", true),
        MakeProperty<T, math::Vector4, &T::colorEnd>("colorEnd", PropertyType::Color, "Color End", "Mesh", true),
        MakeProperty<T, std::string, &T::animatedParam>("animatedParam", PropertyType::String, "Extra Param", "Mesh", true),
        MakeProperty<T, float, &T::paramStart>("paramStart", PropertyType::Float, "Param Start", "Mesh", true),
        MakeProperty<T, float, &T::paramEnd>("paramEnd", PropertyType::Float, "Param End", "Mesh", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXAnimatedMeshSchema()
{
    using T = VFXAnimatedMeshSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXAnimatedMeshSettings", {
        MakeProperty<T, std::string, &T::modelPath>("modelPath", PropertyType::AssetRef, "Model", "Animated Mesh", true),
        MakeProperty<T, std::string, &T::controllerPath>("controllerPath", PropertyType::AssetRef, "Animator Controller", "Animated Mesh", true),
        MakeProperty<T, std::string, &T::materialPath>("materialPath", PropertyType::AssetRef, "Material Override", "Animated Mesh", true),
        MakeProperty<T, std::string, &T::initialState>("initialState", PropertyType::String, "Initial State", "Animated Mesh", true),
        MakeProperty<T, float, &T::speed>("speed", PropertyType::Float, "Speed", "Animated Mesh", true),
        MakeProperty<T, float, &T::startNormalizedTime>("startNormalizedTime", PropertyType::Float, "Start Normalized Time", "Animated Mesh", true, detail::Range(0, 1)),
        MakeProperty<T, int, &T::meshIndex>("meshIndex", PropertyType::Int, "Mesh Index", "Animated Mesh", true),
        MakeProperty<T, bool, &T::loop>("loop", PropertyType::Bool, "Loop", "Animated Mesh", true),
        MakeProperty<T, bool, &T::syncToGraphTime>("syncToGraphTime", PropertyType::Bool, "Sync To Graph Time", "Animated Mesh", true),
        MakeProperty<T, bool, &T::applyRootMotion>("applyRootMotion", PropertyType::Bool, "Apply Root Motion", "Animated Mesh", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXScreenEffectSchema()
{
    using T = VFXScreenEffectSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXScreenEffectSettings", {
        MakeProperty<T, math::Vector3, &T::flashColor>("flashColor", PropertyType::Vector3, "Flash Color", "Screen", true),
        MakeProperty<T, float, &T::flashIntensity>("flashIntensity", PropertyType::Float, "Flash", "Screen", true, detail::Range(0, 1)),
        MakeProperty<T, float, &T::bloomBoost>("bloomBoost", PropertyType::Float, "Bloom Boost", "Screen", true, detail::Range(0, 8)),
        MakeProperty<T, float, &T::chromaticAberration>("chromaticAberration", PropertyType::Float, "Chromatic Aberration", "Screen", true, detail::Range(0, 0.2f)),
        MakeProperty<T, float, &T::lensDistortion>("lensDistortion", PropertyType::Float, "Lens Distortion", "Screen", true, detail::Range(-1, 1)),
        MakeProperty<T, float, &T::vignette>("vignette", PropertyType::Float, "Vignette", "Screen", true, detail::Range(0, 1)),
        MakeProperty<T, float, &T::fadeInTime>("fadeInTime", PropertyType::Float, "Fade In", "Screen", true),
        MakeProperty<T, float, &T::fadeOutTime>("fadeOutTime", PropertyType::Float, "Fade Out", "Screen", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXCameraShakeSchema()
{
    using T = VFXCameraShakeSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXCameraShakeSettings", {
        MakeProperty<T, float, &T::amplitude>("amplitude", PropertyType::Float, "Amplitude", "Shake", true, detail::Range(0, 5)),
        MakeProperty<T, float, &T::rotationAmplitude>("rotationAmplitude", PropertyType::Float, "Rotation Amplitude", "Shake", true, detail::Range(0, 45)),
        MakeProperty<T, float, &T::frequency>("frequency", PropertyType::Float, "Frequency", "Shake", true, detail::Range(0, 60)),
        MakeProperty<T, float, &T::falloffPower>("falloffPower", PropertyType::Float, "Falloff Power", "Shake", true, detail::Range(0.05f, 8)),
        MakeProperty<T, float, &T::radius>("radius", PropertyType::Float, "Radius", "Shake", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXTimeScaleSchema()
{
    using T = VFXTimeScaleSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXTimeScaleSettings", {
        MakeProperty<T, float, &T::timeScale>("timeScale", PropertyType::Float, "Time Scale", "Time", true, detail::Range(0, 2)),
        MakeProperty<T, float, &T::blendInTime>("blendInTime", PropertyType::Float, "Blend In", "Time", true),
        MakeProperty<T, float, &T::blendOutTime>("blendOutTime", PropertyType::Float, "Blend Out", "Time", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXWindSchema()
{
    using T = VFXWindSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXWindSettings", {
        MakeProperty<T, math::Vector3, &T::direction>("direction", PropertyType::Vector3, "Direction", "Wind", true),
        MakeProperty<T, float, &T::strength>("strength", PropertyType::Float, "Strength", "Wind", true),
        MakeProperty<T, float, &T::turbulence>("turbulence", PropertyType::Float, "Turbulence", "Wind", true),
        MakeProperty<T, float, &T::pulseFrequency>("pulseFrequency", PropertyType::Float, "Pulse Frequency", "Wind", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXNodeSchema()
{
    using N = VFXGraphNode; using reflection::MakeProperty; using reflection::MakeStructProperty;
    using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXGraphNode", {
        MakeProperty<N, bool, &N::enabled>("enabled", PropertyType::Bool, "Enabled", "Node", false),
        MakeProperty<N, float, &N::startOffset>("startOffset", PropertyType::Float, "Start Offset", "Timing", true),
        MakeProperty<N, float, &N::duration>("duration", PropertyType::Float, "Duration", "Timing", true),
        MakeProperty<N, math::Vector3, &N::localPosition>("localPosition", PropertyType::Vector3, "Position", "Transform", true),
        MakeProperty<N, math::Vector3, &N::localRotationDegrees>("localRotationDegrees", PropertyType::Vector3, "Rotation", "Transform", true),
        MakeProperty<N, math::Vector3, &N::localScale>("localScale", PropertyType::Vector3, "Scale", "Transform", true),
        MakeProperty<N, std::string, &N::attachBone>("attachBone", PropertyType::String, "Attach Bone / Socket", "Transform", true),
        // 空間の親 (-1 = owner 直下)。実行の因果を表す link とは独立した木。
        MakeProperty<N, int, &N::parentNodeId>("parentNodeId", PropertyType::Int, "Parent Node", "Transform", true),
        MakeStructProperty<N, scene::ParticleEmitter, &N::particle>("particle", "Particle", "Node", GetParticleEmitterSchema()),
        MakeStructProperty<N, VFXTrailSettings, &N::trail>("trail", "Trail", "Node", GetVFXTrailSchema()),
        MakeStructProperty<N, VFXLightSettings, &N::light>("light", "Light", "Node", GetVFXLightSchema()),
        MakeStructProperty<N, VFXAudioSettings, &N::audio>("audio", "Audio", "Node", GetVFXAudioSchema()),
        MakeStructProperty<N, VFXDecalSettings, &N::decal>("decal", "Decal", "Node", GetVFXDecalSchema()),
        MakeStructProperty<N, VFXSubGraphSettings, &N::subGraph>("subGraph", "Sub Graph", "Node", GetVFXSubGraphSchema()),
        MakeStructProperty<N, VFXForceFieldSettings, &N::forceField>("forceField", "Force Field", "Node", GetVFXForceFieldSchema()),
        MakeStructProperty<N, VFXMeshSettings, &N::mesh>("mesh", "Mesh", "Node", GetVFXMeshSchema()),
        MakeStructProperty<N, VFXAnimatedMeshSettings, &N::animatedMesh>("animatedMesh", "Animated Mesh", "Node", GetVFXAnimatedMeshSchema()),
        MakeStructProperty<N, VFXScreenEffectSettings, &N::screenEffect>("screenEffect", "Screen Effect", "Node", GetVFXScreenEffectSchema()),
        MakeStructProperty<N, VFXCameraShakeSettings, &N::cameraShake>("cameraShake", "Camera Shake", "Node", GetVFXCameraShakeSchema()),
        MakeStructProperty<N, VFXTimeScaleSettings, &N::timeScale>("timeScale", "Time Scale", "Node", GetVFXTimeScaleSchema()),
        MakeStructProperty<N, VFXWindSettings, &N::wind>("wind", "Wind", "Node", GetVFXWindSchema()),
    }); return schema;
}

} // namespace fbzz::asset
