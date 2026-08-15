// FBZZ Engine
// PhysicsMaterial.cpp | fbzz::physics
// 剛体の表面物性 (反発・摩擦・密度) のプリセットと合成規則
#include <Physics/PhysicsMaterial.hpp>
#include <algorithm> // std::min, std::max
#include <cmath>     // std::sqrt
#include <cstring>   // std::strcmp

namespace fbzz::physics
{
    float PhysicsMaterial::Combine(PhysicsMaterialCombine mode, float a, float b)
    {
        switch (mode) {
        case PhysicsMaterialCombine::Average:       return (a + b) * 0.5f;
        case PhysicsMaterialCombine::GeometricMean: return std::sqrt(a * b);
        case PhysicsMaterialCombine::Minimum:       return std::min(a, b);
        case PhysicsMaterialCombine::Multiply:      return a * b;
        case PhysicsMaterialCombine::Maximum:       return std::max(a, b);
        }
        return (a + b) * 0.5f;
    }

    PhysicsMaterialCombine PhysicsMaterial::ResolveCombine(PhysicsMaterialCombine a,
                                                          PhysicsMaterialCombine b)
    {
        // 列挙の並び順をそのまま優先度として使う (後ろほど強い)。
        // Average < GeometricMean < Minimum < Multiply < Maximum
        return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
    }

    float PhysicsMaterial::CombineRestitution(const PhysicsMaterial& a, const PhysicsMaterial& b)
    {
        return Combine(ResolveCombine(a.restitutionCombine, b.restitutionCombine),
                       a.restitution, b.restitution);
    }

    float PhysicsMaterial::CombineFriction(const PhysicsMaterial& a, const PhysicsMaterial& b)
    {
        return Combine(ResolveCombine(a.frictionCombine, b.frictionCombine),
                       a.dynamicFriction, b.dynamicFriction);
    }

    float PhysicsMaterial::CombineStaticFriction(const PhysicsMaterial& a, const PhysicsMaterial& b)
    {
        return Combine(ResolveCombine(a.frictionCombine, b.frictionCombine),
                       a.staticFriction, b.staticFriction);
    }

    // restitution, staticFriction, dynamicFriction, density の順。
    // 合成規則は既定 (反発=Minimum / 摩擦=GeometricMean) をそのまま使う。
    const PhysicsMaterial PhysicsMaterial::Default = { 0.3f, 0.6f, 0.4f, 1.0f };
    const PhysicsMaterial PhysicsMaterial::Rubber  = { 0.8f, 1.0f, 0.9f, 1.2f };
    const PhysicsMaterial PhysicsMaterial::Ice     = { 0.05f, 0.05f, 0.02f, 0.9f };
    const PhysicsMaterial PhysicsMaterial::Metal   = { 0.4f, 0.4f, 0.3f, 7.8f };
    const PhysicsMaterial PhysicsMaterial::Wood    = { 0.2f, 0.7f, 0.6f, 0.6f };
    const PhysicsMaterial PhysicsMaterial::Stone   = { 0.1f, 0.9f, 0.8f, 2.5f };

    namespace {
        // 名前とプリセットの対応表。Editor のメニュー順もこの並びに従う。
        const char* const kPresetNames[PhysicsMaterial::PRESET_COUNT] = {
            "Default", "Rubber", "Ice", "Metal", "Wood", "Stone"
        };
        const PhysicsMaterial* const kPresets[PhysicsMaterial::PRESET_COUNT] = {
            &PhysicsMaterial::Default,
            &PhysicsMaterial::Rubber,
            &PhysicsMaterial::Ice,
            &PhysicsMaterial::Metal,
            &PhysicsMaterial::Wood,
            &PhysicsMaterial::Stone,
        };
    }

    const char* PhysicsMaterial::PresetName(int index)
    {
        if (index < 0 || index >= PRESET_COUNT) return "";
        return kPresetNames[index];
    }

    const PhysicsMaterial* PhysicsMaterial::PresetAt(int index)
    {
        if (index < 0 || index >= PRESET_COUNT) return nullptr;
        return kPresets[index];
    }

    const PhysicsMaterial* PhysicsMaterial::FindPreset(const char* name)
    {
        if (!name) return nullptr;
        for (int i = 0; i < PRESET_COUNT; ++i)
            if (std::strcmp(name, kPresetNames[i]) == 0) return kPresets[i];
        return nullptr;
    }
} // namespace fbzz::physics
