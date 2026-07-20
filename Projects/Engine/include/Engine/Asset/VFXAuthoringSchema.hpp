// FBZZ Engine
// VFXAuthoringSchema.hpp | fbzz::asset
// VFXノード設定をInspector・binding・AIで共有するauthoringスキーマ
#pragma once

#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Reflection/TypeSchema.hpp>
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

} // namespace detail

[[nodiscard]] inline const reflection::ITypeSchema& GetParticleEmitterSchema()
{
    using P = scene::ParticleEmitter;
    using reflection::MakeProperty;
    using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("ParticleEmitter", {
        MakeProperty<P, math::Vector3, &P::emitPosition>("emitPosition", PropertyType::Vector3, "Emit Position", "Emission", true),
        MakeProperty<P, math::Vector3, &P::emitVelocity>("emitVelocity", PropertyType::Vector3, "Emit Velocity", "Emission", true),
        MakeProperty<P, float, &P::velocitySpread>("velocitySpread", PropertyType::Float, "Velocity Spread", "Emission", true, detail::Range(0, 100)),
        MakeProperty<P, math::Vector4, &P::colorStart>("colorStart", PropertyType::Color, "Start Color", "Color", true),
        MakeProperty<P, math::Vector4, &P::colorEnd>("colorEnd", PropertyType::Color, "End Color", "Color", true),
        MakeProperty<P, float, &P::sizeStart>("sizeStart", PropertyType::Float, "Start Size", "Main", true, detail::Range(0, 1000)),
        MakeProperty<P, float, &P::sizeEnd>("sizeEnd", PropertyType::Float, "End Size", "Main", true, detail::Range(0, 1000)),
        MakeProperty<P, float, &P::lifetime>("lifetime", PropertyType::Float, "Lifetime", "Main", true, detail::Range(0.001f, 3600)),
        MakeProperty<P, float, &P::lifetimeRandom>("lifetimeRandom", PropertyType::Float, "Lifetime Random", "Main", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::emitRate>("emitRate", PropertyType::Float, "Rate", "Emission", true, detail::Range(0, 1000000)),
        MakeProperty<P, int, &P::maxParticles>("maxParticles", PropertyType::Int, "Max Particles", "Main", true, detail::Range(1, 10000000)),
        MakeProperty<P, math::Vector3, &P::gravity>("gravity", PropertyType::Vector3, "Gravity", "Forces", true),
        MakeProperty<P, bool, &P::loop>("loop", PropertyType::Bool, "Loop", "Main", true),
        MakeProperty<P, float, &P::duration>("duration", PropertyType::Float, "Duration", "Main", true, detail::Range(0, 3600)),
        MakeProperty<P, float, &P::startDelay>("startDelay", PropertyType::Float, "Start Delay", "Main", true, detail::Range(0, 3600)),
        MakeProperty<P, scene::ParticleEmitterShape, &P::shape>("shape", PropertyType::Enum, "Shape", "Shape", false),
        MakeProperty<P, float, &P::sphereRadius>("sphereRadius", PropertyType::Float, "Sphere Radius", "Shape", true, detail::Range(0, 10000)),
        MakeProperty<P, float, &P::coneAngleDegrees>("coneAngleDegrees", PropertyType::Float, "Cone Angle", "Shape", true, detail::Range(0, 180)),
        MakeProperty<P, float, &P::coneRadius>("coneRadius", PropertyType::Float, "Cone Radius", "Shape", true, detail::Range(0, 10000)),
        MakeProperty<P, math::Vector3, &P::boxExtents>("boxExtents", PropertyType::Vector3, "Box Extents", "Shape", true),
        MakeProperty<P, std::string, &P::meshShapePath>("meshShapePath", PropertyType::AssetRef, "Mesh Shape", "Shape", true),
        MakeProperty<P, float, &P::meshShapeScale>("meshShapeScale", PropertyType::Float, "Mesh Scale", "Shape", true, detail::Range(0.001f, 1000)),
        MakeProperty<P, scene::ParticleSimulationMode, &P::simulationMode>("simulationMode", PropertyType::Enum, "Simulation", "Renderer", false),
        MakeProperty<P, scene::ParticleRenderMode, &P::renderMode>("renderMode", PropertyType::Enum, "Render Mode", "Renderer", false),
        MakeProperty<P, float, &P::stretchedVelocityScale>("stretchedVelocityScale", PropertyType::Float, "Velocity Scale", "Renderer", true),
        MakeProperty<P, float, &P::stretchedLengthScale>("stretchedLengthScale", PropertyType::Float, "Length Scale", "Renderer", true),
        MakeProperty<P, std::string, &P::materialPath>("materialPath", PropertyType::AssetRef, "Material", "Renderer", true),
        MakeProperty<P, std::string, &P::meshParticlePath>("meshParticlePath", PropertyType::AssetRef, "Mesh Particle", "Renderer", true),
        MakeProperty<P, std::string, &P::texturePath>("texturePath", PropertyType::AssetRef, "Texture", "Renderer", true),
        MakeProperty<P, int, &P::spriteColumns>("spriteColumns", PropertyType::Int, "Columns", "Flipbook", true, detail::Range(1, 256)),
        MakeProperty<P, int, &P::spriteRows>("spriteRows", PropertyType::Int, "Rows", "Flipbook", true, detail::Range(1, 256)),
        MakeProperty<P, float, &P::flipbookFramesPerSecond>("flipbookFramesPerSecond", PropertyType::Float, "FPS", "Flipbook", true),
        MakeProperty<P, bool, &P::flipbookFrameBlending>("flipbookFrameBlending", PropertyType::Bool, "Frame Blending", "Flipbook", true),
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
        MakeProperty<P, float, &P::rateOverDistance>("rateOverDistance", PropertyType::Float, "Rate Over Distance", "Emission", true),
        MakeProperty<P, bool, &P::prewarm>("prewarm", PropertyType::Bool, "Prewarm", "Emission", true),
        MakeProperty<P, bool, &P::softParticles>("softParticles", PropertyType::Bool, "Soft Particles", "Renderer", true),
        MakeProperty<P, float, &P::softParticleFadeDistance>("softParticleFadeDistance", PropertyType::Float, "Soft Fade", "Renderer", true),
        MakeProperty<P, bool, &P::distortion>("distortion", PropertyType::Bool, "Distortion", "Renderer", true),
        MakeProperty<P, float, &P::distortionStrength>("distortionStrength", PropertyType::Float, "Distortion Strength", "Renderer", true, detail::Range(0, 0.25f)),
        MakeProperty<P, bool, &P::sixWayLighting>("sixWayLighting", PropertyType::Bool, "Six-way Lit Smoke", "Renderer", true),
        MakeProperty<P, float, &P::lightingStrength>("lightingStrength", PropertyType::Float, "Lighting Strength", "Renderer", true, detail::Range(0, 8)),
        MakeProperty<P, float, &P::emissiveScale>("emissiveScale", PropertyType::Float, "HDR Emissive", "Renderer", true, detail::Range(0, 100)),
        MakeProperty<P, bool, &P::lodEnabled>("lodEnabled", PropertyType::Bool, "LOD", "LOD", true),
        MakeProperty<P, float, &P::lodNearDistance>("lodNearDistance", PropertyType::Float, "Near Distance", "LOD", true),
        MakeProperty<P, float, &P::lodFarDistance>("lodFarDistance", PropertyType::Float, "Far Distance", "LOD", true),
        MakeProperty<P, float, &P::lodFarRateScale>("lodFarRateScale", PropertyType::Float, "Far Rate", "LOD", true, detail::Range(0, 1)),
        MakeProperty<P, float, &P::noiseStrength>("noiseStrength", PropertyType::Float, "Strength", "Noise", true),
        MakeProperty<P, float, &P::noiseFrequency>("noiseFrequency", PropertyType::Float, "Frequency", "Noise", true),
        MakeProperty<P, float, &P::noiseSpeed>("noiseSpeed", PropertyType::Float, "Speed", "Noise", true),
    });
    return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXTrailSchema()
{
    using T = VFXTrailSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXTrailSettings", {
        MakeProperty<T, std::string, &T::meshPath>("meshPath", PropertyType::AssetRef, "Mesh", "Asset", true),
        MakeProperty<T, std::string, &T::materialPath>("materialPath", PropertyType::AssetRef, "Material", "Asset", true),
        MakeProperty<T, std::string, &T::texturePath>("texturePath", PropertyType::AssetRef, "Texture", "Asset", true),
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
        MakeProperty<T, std::string, &T::albedoPath>("albedoPath", PropertyType::AssetRef, "Albedo", "Decal", true),
        MakeProperty<T, std::string, &T::normalPath>("normalPath", PropertyType::AssetRef, "Normal", "Decal", true),
        MakeProperty<T, std::string, &T::emissivePath>("emissivePath", PropertyType::AssetRef, "Emissive", "Decal", true),
        MakeProperty<T, math::Vector4, &T::color>("color", PropertyType::Color, "Color", "Decal", true),
        MakeProperty<T, float, &T::normalStrength>("normalStrength", PropertyType::Float, "Normal Strength", "Decal", true),
        MakeProperty<T, float, &T::emissiveScale>("emissiveScale", PropertyType::Float, "Emissive Scale", "Decal", true),
        MakeProperty<T, float, &T::fadeTime>("fadeTime", PropertyType::Float, "Fade Time", "Decal", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXSubGraphSchema()
{
    using T = VFXSubGraphSettings; using reflection::MakeProperty; using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXSubGraphSettings", {
        MakeProperty<T, std::string, &T::graphPath>("graphPath", PropertyType::AssetRef, "Graph", "Sub Graph", true),
    }); return schema;
}

[[nodiscard]] inline const reflection::ITypeSchema& GetVFXNodeSchema()
{
    using N = VFXGraphNode; using reflection::MakeProperty; using reflection::MakeStructProperty;
    using reflection::PropertyType;
    static const detail::StaticTypeSchema schema("VFXGraphNode", {
        MakeProperty<N, float, &N::startOffset>("startOffset", PropertyType::Float, "Start Offset", "Timing", true),
        MakeProperty<N, float, &N::duration>("duration", PropertyType::Float, "Duration", "Timing", true),
        MakeProperty<N, math::Vector3, &N::localPosition>("localPosition", PropertyType::Vector3, "Position", "Transform", true),
        MakeProperty<N, math::Vector3, &N::localRotationDegrees>("localRotationDegrees", PropertyType::Vector3, "Rotation", "Transform", true),
        MakeProperty<N, math::Vector3, &N::localScale>("localScale", PropertyType::Vector3, "Scale", "Transform", true),
        MakeProperty<N, std::string, &N::attachBone>("attachBone", PropertyType::String, "Attach Bone / Socket", "Transform", true),
        MakeStructProperty<N, scene::ParticleEmitter, &N::particle>("particle", "Particle", "Node", GetParticleEmitterSchema()),
        MakeStructProperty<N, VFXTrailSettings, &N::trail>("trail", "Trail", "Node", GetVFXTrailSchema()),
        MakeStructProperty<N, VFXLightSettings, &N::light>("light", "Light", "Node", GetVFXLightSchema()),
        MakeStructProperty<N, VFXAudioSettings, &N::audio>("audio", "Audio", "Node", GetVFXAudioSchema()),
        MakeStructProperty<N, VFXDecalSettings, &N::decal>("decal", "Decal", "Node", GetVFXDecalSchema()),
        MakeStructProperty<N, VFXSubGraphSettings, &N::subGraph>("subGraph", "Sub Graph", "Node", GetVFXSubGraphSchema()),
    }); return schema;
}

} // namespace fbzz::asset
